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
#include <unordered_map>
#include <utility>
#include <vector>

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
     * Type alias for LruCacheResult<std::vector<unsigned char>>.
     * Use `.value` to access the UID and `.found` to check if it existed in cache.
     */
    using SqlUidCacheResult = LruCacheResult<std::vector<unsigned char>>;

    /**
     * @brief Thread-safe LRU cache implementation template.
     * 
     * Provides O(1) lookup, insertion, and removal using a combination of
     * std::list (for LRU ordering) and std::unordered_map (for fast lookup).
     * 
     * @tparam ValueType Type of values stored in the cache.
     */
    template<typename ValueType>
    class LruCacheImpl {
    public:
        explicit LruCacheImpl(size_t max_size) : max_size_(max_size) {}
        ~LruCacheImpl() = default;

        // Delete copy and move operations (mutex is not movable)
        LruCacheImpl(const LruCacheImpl&) = delete;
        LruCacheImpl& operator=(const LruCacheImpl&) = delete;
        LruCacheImpl(LruCacheImpl&&) = delete;
        LruCacheImpl& operator=(LruCacheImpl&&) = delete;

        /**
         * @brief Retrieves or creates a cache entry.
         * 
         * @param key The key to look up.
         * @param generator Function to generate a new value if key not found.
         * @return Result containing the value and whether it was found.
         */
        template<typename Generator>
        LruCacheResult<ValueType> get(const std::string& key, Generator&& generator) {
            std::unique_lock<std::mutex> lock(mutex_);

            const auto it = cache_map_.find(key);
            if (it != cache_map_.end()) {
                // Move accessed item to front (most recently used)
                cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
                return LruCacheResult<ValueType>{it->second->second, true};
            }

            // Not found - generate new value and insert
            auto new_value = generator();
            put(key, std::move(new_value));
            return LruCacheResult<ValueType>{cache_list_.front().second, false};
        }

        /**
         * @brief Removes an entry from the cache.
         * 
         * @param key The key to remove.
         */
        void remove(const std::string& key) {
            std::unique_lock<std::mutex> lock(mutex_);

            const auto it = cache_map_.find(key);
            if (it != cache_map_.end()) {
                cache_list_.erase(it->second);
                cache_map_.erase(it);
            }
        }

    private:
        /**
         * @brief Inserts a new key/value pair while preserving LRU ordering.
         * 
         * Assumes the lock is already held by the caller.
         * 
         * @param key The key to insert.
         * @param value The value to insert (moved).
         */
        void put(const std::string& key, ValueType&& value) {
            // Insert new entry at front
            cache_list_.emplace_front(key, std::move(value));
            
            // Exception-safe: if emplace fails, we won't update the map
            try {
                cache_map_.emplace(key, cache_list_.begin());
            } catch (...) {
                cache_list_.pop_front();  // Rollback
                throw;
            }

            // Evict least recently used entry if over capacity
            if (cache_map_.size() > max_size_) {
                const auto& victim_key = cache_list_.back().first;
                cache_map_.erase(victim_key);
                cache_list_.pop_back();
            }
        }

        using KeyValuePair = std::pair<std::string, ValueType>;
        std::list<KeyValuePair> cache_list_{};
        std::unordered_map<std::string, typename std::list<KeyValuePair>::iterator> cache_map_{};
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
         * @param key String to cache.
         * @return CacheResult containing the identifier and whether the entry already existed.
         */
        CacheResult get(const std::string& key);

        /**
         * @brief Evicts a cached string from the cache.
         *
         * @param key Entry to remove.
         */
        void remove(const std::string& key) {
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
         * @param key Normalized SQL string.
         * @return Cache result containing UID bytes and whether the entry existed.
         */
        SqlUidCacheResult get(const std::string& key);

        /**
         * @brief Removes a cached SQL UID entry.
         *
         * @param key Normalized SQL string.
         */
        void remove(const std::string& key) {
            cache_.remove(key);
        }

    private:
        LruCacheImpl<std::vector<unsigned char>> cache_;
    };

} // namespace pinpoint
