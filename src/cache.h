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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "utility.h"

namespace pinpoint {

    enum class SqlMetaMode : uint8_t {
        Id,
        Uid,
    };

    using SqlIdentity = std::variant<int32_t, SqlUid>;

    /**
     * Immutable result of preparing one raw SQL statement. RawSqlCache returns
     * shared ownership so annotations can refer to the extracted parameters
     * without copying them, even after the cache entry itself is evicted.
     */
    struct PreparedSql {
        std::string normalized_sql;
        std::string parameters;
        SqlIdentity identity;
    };

    using PreparedSqlRef = std::shared_ptr<const PreparedSql>;

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
    private:
        // Declared before the member functions: insert_or_promote() names
        // Node in its parameter list, which — unlike function bodies — is
        // not complete-class context, so a later declaration would not be
        // found there (and could resolve to an unrelated outer Node type).
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

            // Stage the new node outside the lock as well: the list-node
            // allocation and KeyTraits::store()'s key copy (for RawSqlCache
            // the full raw SQL text, up to 64 KB) would otherwise stall every
            // concurrent shared-lock hit behind malloc + memcpy. Taking the
            // op sequence here (relaxed, like every op_seq_ use) can reorder
            // it slightly against promotions racing under the lock; that only
            // perturbs an entry's age estimate by a step — the same benign
            // race the shared-lock age check above already tolerates.
            // `staged` doubles as the graveyard: the node discarded on a lost
            // insert race and the evicted victim are spliced back onto it, so
            // their destruction also runs after the lock is released.
            std::list<Node> staged;
            staged.emplace_front(KeyTraits::store(key), std::move(new_value),
                                 next_op_seq());

            std::unique_lock<std::shared_mutex> lock(mutex_);
            return insert_or_promote(staged);
        }

        /**
         * @brief Removes an entry from the cache.
         *
         * @param key The key to remove.
         */
        void remove(LookupKey key) {
            // Declared before the lock so the removed node (key storage and
            // value) is destroyed after the exclusive section, not while
            // shared-lock readers wait behind the free.
            std::list<Node> removed;
            std::unique_lock<std::shared_mutex> lock(mutex_);

            const auto it = cache_map_.find(KeyTraits::lookup_key(key));
            if (it != cache_map_.end()) {
                removed.splice(removed.begin(), cache_list_, it->second);
                cache_map_.erase(it);
            }
        }

    private:
        /**
         * @brief Inserts the staged node, or promotes an existing entry.
         *
         * The candidate node is pre-built by get() in `staged` — outside the
         * lock — so this critical section only relinks list pointers and
         * updates the map; it never allocates, copies key bytes, or destroys
         * a node (the map's buckets are pre-reserved in the ctor, so
         * try_emplace cannot rehash either). Because the generator ran
         * outside the lock, another thread may have inserted the same key in
         * the meantime. We detect that race via try_emplace and, if so,
         * leave our node in `staged` — the caller frees it after unlocking —
         * and return the existing entry (found = true). The evicted victim
         * is spliced onto `staged` for the same deferred destruction.
         *
         * Hashes the key once: try_emplace performs the existence check and
         * the insert in a single map operation. The map key is a view into
         * the node's owned key storage, which splice never relocates.
         * Assumes the lock is already held by the caller.
         *
         * @param staged Single-node list holding the candidate; receives the
         *               discarded or evicted node, if any.
         */
        LruCacheResult<ValueType> insert_or_promote(std::list<Node>& staged) {
            const auto list_it = staged.begin();

            // No rollback needed if this throws: the candidate still lives in
            // `staged`, and neither the map nor the list has changed.
            const auto inserted =
                cache_map_.try_emplace(KeyTraits::map_key(list_it->key), list_it);

            if (!inserted.second) {
                // Lost the race: an identical key was inserted concurrently.
                // Leave our node in staged and promote the existing entry to
                // most-recently-used, reusing the op sequence the candidate
                // was stamped with.
                cache_list_.splice(cache_list_.begin(), cache_list_, inserted.first->second);
                inserted.first->second->last_promoted.store(
                    list_it->last_promoted.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                return LruCacheResult<ValueType>{inserted.first->second->value, true};
            }

            // Adopt the staged node at the MRU end: an O(1) pointer relink.
            cache_list_.splice(cache_list_.begin(), staged, list_it);

            // Evict the least recently used entry if over capacity. Only the
            // map erase happens here; the node itself — its key string and
            // value, possibly the last PreparedSqlRef — moves to staged and
            // is freed by the caller after unlock.
            if (cache_map_.size() > max_size_) {
                const auto victim = std::prev(cache_list_.end());
                cache_map_.erase(KeyTraits::map_key(victim->key));
                staged.splice(staged.end(), cache_list_, victim);
            }
            return LruCacheResult<ValueType>{list_it->value, false};
        }

        uint64_t next_op_seq() noexcept {
            return op_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
        }

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

    struct RawSqlCacheKey {
        std::string_view raw_sql;
        uint64_t metadata_epoch;
        size_t hash;
    };

    struct RawSqlCacheStoredKey {
        std::string raw_sql;
        uint64_t metadata_epoch;
        size_t hash;
    };

    struct RawSqlCacheKeyHash {
        size_t operator()(const RawSqlCacheKey& key) const noexcept {
            return key.hash;
        }
    };

    struct RawSqlCacheKeyEqual {
        bool operator()(const RawSqlCacheKey& lhs, const RawSqlCacheKey& rhs) const noexcept {
            return lhs.metadata_epoch == rhs.metadata_epoch && lhs.raw_sql == rhs.raw_sql;
        }
    };

    struct RawSqlCacheKeyTraits {
        using LookupKey = RawSqlCacheKey;
        using StoredKey = RawSqlCacheStoredKey;
        using MapKey = RawSqlCacheKey;
        using Hash = RawSqlCacheKeyHash;
        using Equal = RawSqlCacheKeyEqual;

        static MapKey lookup_key(LookupKey key) noexcept {
            return key;
        }

        static StoredKey store(LookupKey key) {
            return RawSqlCacheStoredKey{
                std::string(key.raw_sql), key.metadata_epoch, key.hash};
        }

        static MapKey map_key(const StoredKey& key) noexcept {
            return RawSqlCacheKey{
                key.raw_sql, key.metadata_epoch, key.hash};
        }
    };

    using RawSqlCacheResult = LruCacheResult<PreparedSqlRef>;

    /**
     * Front cache keyed by raw SQL. The cache is sharded so unrelated hot SQL
     * statements do not contend on one shared_mutex. A hash is computed once
     * per lookup and carried in the heterogeneous key, avoiding a second scan
     * of the SQL text inside unordered_map.
     */
    class RawSqlCache {
    public:
        explicit RawSqlCache(size_t max_size,
                             size_t shard_count = 16,
                             size_t max_cacheable_length = 64 * 1024)
            : max_cacheable_length_(max_cacheable_length) {
            const size_t entry_count = max_size > 0 ? max_size : 1;
            const size_t requested_shards = shard_count > 0 ? shard_count : 1;
            const size_t actual_shards = std::min(requested_shards, entry_count);
            const size_t base_size = entry_count / actual_shards;
            const size_t remainder = entry_count % actual_shards;

            shards_.reserve(actual_shards);
            for (size_t i = 0; i < actual_shards; ++i) {
                const size_t capacity = base_size + (i < remainder ? 1 : 0);
                shards_.emplace_back(std::make_unique<Shard>(capacity));
            }
        }

        RawSqlCache(const RawSqlCache&) = delete;
        RawSqlCache& operator=(const RawSqlCache&) = delete;
        RawSqlCache(RawSqlCache&&) = delete;
        RawSqlCache& operator=(RawSqlCache&&) = delete;

        template<typename Generator>
        RawSqlCacheResult get(std::string_view raw_sql,
                              uint64_t metadata_epoch,
                              Generator&& generator) {
            // Very large statements remain correct but bypass the cache so one
            // pathological key cannot pin an excessive amount of memory.
            if (raw_sql.size() > max_cacheable_length_) {
                return RawSqlCacheResult{generator(), false};
            }

            size_t hash = std::hash<std::string_view>{}(raw_sql);
            hash ^= std::hash<uint64_t>{}(metadata_epoch) + 0x9e3779b9 +
                    (hash << 6) + (hash >> 2);
            // Pick the shard from a remixed value, not `hash % size`: the
            // shard's unordered_map is fed this same `hash` (pass-through
            // hasher), so selecting the shard from the low bits would leave
            // every key in a shard sharing those bits. Harmless with prime
            // bucket counts (libstdc++/libc++) but 16x chain length with
            // power-of-two buckets (MSVC). The multiply spreads high entropy
            // into the bits the modulo consumes.
            const size_t shard_idx =
                (hash * 0x9E3779B97F4A7C15ull >> 32) % shards_.size();
            auto& shard = *shards_[shard_idx];
            return shard.cache.get(
                RawSqlCacheKey{raw_sql, metadata_epoch, hash},
                std::forward<Generator>(generator));
        }

        size_t shardCount() const noexcept {
            return shards_.size();
        }

    private:
        struct Shard {
            explicit Shard(size_t capacity) : cache(capacity) {}
            LruCacheImpl<PreparedSqlRef, RawSqlCacheKeyTraits> cache;
        };

        std::vector<std::unique_ptr<Shard>> shards_;
        const size_t max_cacheable_length_;
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
