/*
* Copyright 2020-present NAVER Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utility.h"

namespace pinpoint {

    /**
     * @brief Generic LRU cache result structure.
     *
     * @tparam ValueType Type of the cached value.
     */
    template<typename ValueType>
    struct LruCacheResult {
        ValueType value;
        bool found;  // true if entry already existed in cache
    };

    /**
     * @brief Result returned from `IdCache::get`.
     *
     * Type alias for LruCacheResult<int32_t>.
     * Use `.value` to access the ID and `.found` to check if it existed in cache.
     */
    using CacheResult = LruCacheResult<int32_t>;

    struct ApiCacheKey {
        std::string_view api_str;
        int32_t api_type;
    };

    struct ApiCacheStoredKey {
        std::string api_str;
        int32_t api_type;
    };

    struct ApiCacheKeyHash {
        size_t operator()(const ApiCacheKey& key) const noexcept {
            size_t seed = std::hash<std::string_view>{}(key.api_str);
            seed ^= std::hash<int32_t>{}(key.api_type) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

    struct ApiCacheKeyEqual {
        bool operator()(const ApiCacheKey& lhs, const ApiCacheKey& rhs) const noexcept {
            return lhs.api_type == rhs.api_type && lhs.api_str == rhs.api_str;
        }
    };

    struct StringCacheKeyTraits {
        using LookupKey = std::string_view;
        using StoredKey = std::string;
        using MapKey = std::string_view;
        using Hash = std::hash<MapKey>;
        using Equal = std::equal_to<MapKey>;

        static MapKey lookup_key(LookupKey key) noexcept {
            return key;
        }

        static StoredKey store(LookupKey key) {
            return std::string(key);
        }

        static MapKey map_key(const StoredKey& key) noexcept {
            return std::string_view(key);
        }
    };

    struct ApiCacheKeyTraits {
        using LookupKey = ApiCacheKey;
        using StoredKey = ApiCacheStoredKey;
        using MapKey = ApiCacheKey;
        using Hash = ApiCacheKeyHash;
        using Equal = ApiCacheKeyEqual;

        static MapKey lookup_key(LookupKey key) noexcept {
            return key;
        }

        static StoredKey store(LookupKey key) {
            return ApiCacheStoredKey{std::string(key.api_str), key.api_type};
        }

        static MapKey map_key(const StoredKey& key) noexcept {
            return ApiCacheKey{key.api_str, key.api_type};
        }
    };

    /**
     * @brief Result returned from `SqlUidCache::get`.
     *
     * Type alias for LruCacheResult<SqlUid>. SqlUid is a fixed 16-byte array, so
     * copying a result out of the cache on a hit is allocation-free.
     * Use `.value` to access the UID and `.found` to check if it existed in cache.
     */
    using SqlUidCacheResult = LruCacheResult<SqlUid>;

    /**
     * @brief Thread-safe LRU cache implementation template.
     *
     * Combines a std::list (LRU ordering) with a std::unordered_map keyed by
     * a non-owning map key from KeyTraits (O(1) lookup, no allocation on the hit path).
     * Access is guarded by a std::shared_mutex: lookups take a shared lock so
     * cache hits run concurrently. LRU reordering is performed lazily — while the
     * cache is below max_size no eviction can occur, so ordering is irrelevant;
     * once full, an entry is promoted only after it has aged past half the
     * capacity's churn (see get()), so most hits remain pure shared-lock reads
     * instead of serializing on the exclusive lock the splice needs.
     *
     * @tparam ValueType Type of values stored in the cache.
     * @tparam KeyTraits Converts lookup keys into owned storage and map keys.
     */
    template<typename ValueType, typename KeyTraits = StringCacheKeyTraits>
    class LruCacheImpl {
    public:
        using LookupKey = typename KeyTraits::LookupKey;

        // max_size is clamped to >= 1: with a capacity of 0 the eviction in
        // insert_or_promote() would erase the just-inserted front node and
        // then return a reference into the freed list node — use-after-free.
        explicit LruCacheImpl(size_t max_size)
            : max_size_(max_size > 0 ? max_size : 1),
              promote_age_threshold_(max_size_ / 2 > 0 ? max_size_ / 2 : 1) {
            // Reserve buckets up front so the map never rehashes while warming
            // up to capacity. +1 covers the transient over-capacity entry that
            // exists between insertion and eviction inside insert_or_promote().
            cache_map_.reserve(max_size_ + 1);
        }
        ~LruCacheImpl() = default;

        // Delete copy and move operations (mutex is not movable)
        LruCacheImpl(const LruCacheImpl&) = delete;
        LruCacheImpl& operator=(const LruCacheImpl&) = delete;
        LruCacheImpl(LruCacheImpl&&) = delete;
        LruCacheImpl& operator=(LruCacheImpl&&) = delete;

        /**
         * @brief Retrieves or creates a cache entry.
         *
         * Lookups take a shared lock, so cache hits run concurrently. While the
         * cache has not reached capacity no entry can be evicted, so LRU ordering
         * is irrelevant and the splice is skipped entirely — a hit is then a pure
         * shared-lock read. Once full, promoting on every hit would serialize all
         * lookups on the exclusive lock the splice needs, so promotion is aged:
         * an entry promoted within the last promote_age_threshold_ insert/promote
         * operations is still far from the eviction end and its hit stays a pure
         * shared-lock read; only entries past that age pay for the splice. On a
         * miss the generator runs OUTSIDE any lock, so an expensive generator
         * does not serialize other threads' lookups.
         *
         * @param key The key to look up (no allocation on hit).
         * @param generator Function to generate a new value if key not found.
         * @return Result containing the value and whether it was found.
         */
        template<typename Generator>
        LruCacheResult<ValueType> get(LookupKey key, Generator&& generator) {
            const auto map_key = KeyTraits::lookup_key(key);
            bool hit_while_full = false;
            {
                // Fast path: a shared lock lets concurrent hits proceed in parallel.
                std::shared_lock<std::shared_mutex> lock(mutex_);
                const auto it = cache_map_.find(map_key);
                if (it != cache_map_.end()) {
                    if (cache_map_.size() < max_size_) {
                        // Below capacity: nothing can be evicted, so LRU order does
                        // not matter — skip the splice and keep this a pure read.
                        return LruCacheResult<ValueType>{it->second->value, true};
                    }
                    // A stale op_seq_ read races benignly with concurrent
                    // promotions: worst case the age is overestimated and the
                    // entry takes one unnecessary trip through the slow path.
                    const auto age = op_seq_.load(std::memory_order_relaxed) -
                                     it->second->last_promoted.load(std::memory_order_relaxed);
                    if (age < promote_age_threshold_) {
                        // Promoted recently: still in the newer half of the LRU
                        // list, so eviction is not imminent — skip the splice.
                        return LruCacheResult<ValueType>{it->second->value, true};
                    }
                    hit_while_full = true;
                }
            }

            if (hit_while_full) {
                // Cache is full and the entry has aged: promote it so it survives
                // the next evictions. Splicing mutates the list and needs an
                // exclusive lock (a shared lock cannot be upgraded). The entry may
                // have been evicted between the two locks, so re-resolve; if it is
                // gone, fall through to regenerate it below.
                std::unique_lock<std::shared_mutex> lock(mutex_);
                const auto it = cache_map_.find(map_key);
                if (it != cache_map_.end()) {
                    cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
                    it->second->last_promoted.store(next_op_seq(), std::memory_order_relaxed);
                    return LruCacheResult<ValueType>{it->second->value, true};
                }
            }

            // Cache miss: run the (potentially expensive) generator WITHOUT holding
            // the lock. For SqlUidCache this is a MurmurHash over the full SQL text,
            // which would otherwise serialize every concurrent lookup.
            auto new_value = generator();

            std::unique_lock<std::shared_mutex> lock(mutex_);
            return insert_or_promote(key, std::move(new_value));
        }

        /**
         * @brief Removes an entry from the cache.
         *
         * @param key The key to remove.
         */
        void remove(LookupKey key) {
            std::unique_lock<std::shared_mutex> lock(mutex_);

            const auto it = cache_map_.find(KeyTraits::lookup_key(key));
            if (it != cache_map_.end()) {
                cache_list_.erase(it->second);
                cache_map_.erase(it);
            }
        }

    private:
        /**
         * @brief Inserts a freshly generated entry, or promotes an existing one.
         *
         * Because the generator runs outside the lock (see get()), another thread
         * may have inserted the same key in the meantime. We detect that race via
         * try_emplace and, if so, discard our value and return the existing entry
         * (found = true). Assumes the lock is already held by the caller.
         *
         * Hashes the key once: try_emplace performs the existence check and the
         * insert in a single map operation.
         *
         * @param key The key to insert (copied into storage only when inserting).
         * @param value The freshly generated value (moved).
         */
        LruCacheResult<ValueType> insert_or_promote(LookupKey key, ValueType&& value) {
            const auto seq = next_op_seq();

            // Speculatively create the list node first so the map key can be a
            // view into the node's owned key storage (single key allocation).
            cache_list_.emplace_front(KeyTraits::store(key), std::move(value), seq);
            const auto list_it = cache_list_.begin();

            std::pair<typename MapType::iterator, bool> inserted;
            try {
                inserted = cache_map_.try_emplace(KeyTraits::map_key(list_it->key), list_it);
            } catch (...) {
                cache_list_.pop_front();  // Rollback the speculative node
                throw;
            }

            if (!inserted.second) {
                // Lost the race: an identical key was inserted concurrently. Drop
                // our node and promote the existing entry to most-recently-used.
                cache_list_.pop_front();
                cache_list_.splice(cache_list_.begin(), cache_list_, inserted.first->second);
                inserted.first->second->last_promoted.store(seq, std::memory_order_relaxed);
                return LruCacheResult<ValueType>{inserted.first->second->value, true};
            }

            // Evict least recently used entry if over capacity
            if (cache_map_.size() > max_size_) {
                cache_map_.erase(KeyTraits::map_key(cache_list_.back().key));
                cache_list_.pop_back();
            }
            return LruCacheResult<ValueType>{list_it->value, false};
        }

        uint64_t next_op_seq() noexcept {
            return op_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        struct Node {
            typename KeyTraits::StoredKey key;
            ValueType value;
            // Op sequence at insert / last promotion. Written under the exclusive
            // lock but read under shared locks, hence atomic (relaxed suffices —
            // a stale read only mis-estimates the age; see get()).
            std::atomic<uint64_t> last_promoted;

            Node(typename KeyTraits::StoredKey&& k, ValueType&& v, uint64_t seq)
                : key(std::move(k)), value(std::move(v)), last_promoted(seq) {}
        };

        using MapType = std::unordered_map<typename KeyTraits::MapKey,
                                          typename std::list<Node>::iterator,
                                          typename KeyTraits::Hash,
                                          typename KeyTraits::Equal>;
        std::list<Node> cache_list_{};
        MapType cache_map_{};
        const size_t max_size_{};
        // A full-cache hit is promoted only once the entry has aged past this
        // many insert/promotion operations; younger entries are still far from
        // the eviction end, so their hits stay pure shared-lock reads.
        const uint64_t promote_age_threshold_{};
        // Counts inserts and promotions; entry age is measured against it.
        std::atomic<uint64_t> op_seq_{0};
        mutable std::shared_mutex mutex_{};
    };

    /**
     * @brief LRU cache that assigns numeric identifiers to frequently used keys.
     *
     * The cache is used for API, SQL, and error metadata to minimize payload sizes
     * when sending data over gRPC.
     */
    template<typename KeyTraits = StringCacheKeyTraits>
    class IdCacheImpl {
    public:
        using LookupKey = typename KeyTraits::LookupKey;

        explicit IdCacheImpl(size_t max_size) : cache_(max_size) {}
        ~IdCacheImpl() = default;

        // Delete copy and move operations
        IdCacheImpl(const IdCacheImpl&) = delete;
        IdCacheImpl& operator=(const IdCacheImpl&) = delete;
        IdCacheImpl(IdCacheImpl&&) = delete;
        IdCacheImpl& operator=(IdCacheImpl&&) = delete;

        /**
         * @brief Looks up or inserts a key identifier.
         *
         * @param key Key to cache (no allocation on cache hit).
         * @return CacheResult containing the identifier and whether the entry already existed.
         */
        CacheResult get(LookupKey key) {
            return cache_.get(key, [this]() {
                return ++id_sequence_;
            });
        }

        /**
         * @brief Evicts a cached key from the cache.
         *
         * @param key Entry to remove.
         */
        void remove(LookupKey key) {
            cache_.remove(key);
        }

    private:
        LruCacheImpl<int32_t, KeyTraits> cache_;
        std::atomic<int32_t> id_sequence_{0};
    };

    using IdCache = IdCacheImpl<StringCacheKeyTraits>;
    using ApiIdCache = IdCacheImpl<ApiCacheKeyTraits>;

    /**
     * @brief LRU cache that assigns binary UIDs to normalized SQL statements.
     */
    class SqlUidCache {
    public:
        explicit SqlUidCache(size_t max_size) : cache_(max_size) {}
        ~SqlUidCache() = default;

        // Delete copy and move operations
        SqlUidCache(const SqlUidCache&) = delete;
        SqlUidCache& operator=(const SqlUidCache&) = delete;
        SqlUidCache(SqlUidCache&&) = delete;
        SqlUidCache& operator=(SqlUidCache&&) = delete;

        /**
         * @brief Looks up or inserts an SQL UID entry.
         *
         * @param key Normalized SQL string (no allocation on cache hit).
         * @return Cache result containing UID bytes and whether the entry existed.
         */
        SqlUidCacheResult get(std::string_view key);

        /**
         * @brief Removes a cached SQL UID entry.
         *
         * @param key Normalized SQL string.
         */
        void remove(std::string_view key) {
            cache_.remove(key);
        }

    private:
        LruCacheImpl<SqlUid> cache_;
    };

} // namespace pinpoint
