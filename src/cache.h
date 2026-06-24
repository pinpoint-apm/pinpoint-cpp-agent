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
#include <list>
#include <mutex>
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
     * Provides O(1) lookup, insertion, and removal using a combination of
     * std::list (for LRU ordering) and std::unordered_map (for fast lookup).
     * Uses transparent hash/equal to allow lookup by std::string_view without
     * allocating a std::string on the hot (cache-hit) path.
     *
     * @tparam ValueType Type of values stored in the cache.
     */
    template<typename ValueType>
    class LruCacheImpl {
    public:
        explicit LruCacheImpl(size_t max_size) : max_size_(max_size) {
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
         * On cache hit the lookup is performed with string_view (zero allocation)
         * and the key is hashed exactly once. On a miss the generator runs
         * OUTSIDE the lock, so an expensive generator does not serialize other
         * threads' lookups; the key string is also allocated off the critical
         * section.
         *
         * @param key The key to look up (string_view — no allocation on hit).
         * @param generator Function to generate a new value if key not found.
         * @return Result containing the value and whether it was found.
         */
        template<typename Generator>
        LruCacheResult<ValueType> get(std::string_view key, Generator&& generator) {
            {
                // Fast path: on a hit we only hash the key once and bump LRU order.
                std::unique_lock<std::mutex> lock(mutex_);
                const auto it = cache_map_.find(key);
                if (it != cache_map_.end()) {
                    cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
                    return LruCacheResult<ValueType>{it->second->second, true};
                }
            }

            // Cache miss: run the (potentially expensive) generator WITHOUT holding
            // the lock. For SqlUidCache this is a MurmurHash over the full SQL text,
            // which would otherwise serialize every concurrent lookup.
            auto new_value = generator();

            std::unique_lock<std::mutex> lock(mutex_);
            return insert_or_promote(key, std::move(new_value));
        }

        /**
         * @brief Removes an entry from the cache.
         *
         * @param key The key to remove.
         */
        void remove(std::string_view key) {
            std::unique_lock<std::mutex> lock(mutex_);

            const auto it = cache_map_.find(key);
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
        LruCacheResult<ValueType> insert_or_promote(std::string_view key, ValueType&& value) {
            // Speculatively create the list node first so the map key can be a
            // string_view into the node's owned string (single key allocation).
            cache_list_.emplace_front(std::string(key), std::move(value));
            const auto list_it = cache_list_.begin();

            std::pair<typename MapType::iterator, bool> inserted;
            try {
                inserted = cache_map_.try_emplace(std::string_view(list_it->first), list_it);
            } catch (...) {
                cache_list_.pop_front();  // Rollback the speculative node
                throw;
            }

            if (!inserted.second) {
                // Lost the race: an identical key was inserted concurrently. Drop
                // our node and promote the existing entry to most-recently-used.
                cache_list_.pop_front();
                cache_list_.splice(cache_list_.begin(), cache_list_, inserted.first->second);
                return LruCacheResult<ValueType>{inserted.first->second->second, true};
            }

            // Evict least recently used entry if over capacity
            if (cache_map_.size() > max_size_) {
                cache_map_.erase(std::string_view(cache_list_.back().first));
                cache_list_.pop_back();
            }
            return LruCacheResult<ValueType>{list_it->second, false};
        }

        using KeyValuePair = std::pair<std::string, ValueType>;
        using MapType = std::unordered_map<std::string_view, typename std::list<KeyValuePair>::iterator>;
        std::list<KeyValuePair> cache_list_{};
        MapType cache_map_{};
        const size_t max_size_{};
        mutable std::mutex mutex_{};
    };

    /**
     * @brief LRU cache that assigns numeric identifiers to frequently used strings.
     *
     * The cache is used for API and error metadata to minimize payload sizes when
     * sending data over gRPC.
     */
    class IdCache {
    public:
        explicit IdCache(size_t max_size) : cache_(max_size) {}
        ~IdCache() = default;

        // Delete copy and move operations
        IdCache(const IdCache&) = delete;
        IdCache& operator=(const IdCache&) = delete;
        IdCache(IdCache&&) = delete;
        IdCache& operator=(IdCache&&) = delete;

        /**
         * @brief Looks up or inserts a string identifier.
         *
         * @param key String to cache (no allocation on cache hit).
         * @return CacheResult containing the identifier and whether the entry already existed.
         */
        CacheResult get(std::string_view key);

        /**
         * @brief Evicts a cached string from the cache.
         *
         * @param key Entry to remove.
         */
        void remove(std::string_view key) {
            cache_.remove(key);
        }

    private:
        LruCacheImpl<int32_t> cache_;
        std::atomic<int32_t> id_sequence_{0};
    };

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
