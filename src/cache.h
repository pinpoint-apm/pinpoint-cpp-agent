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

#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pinpoint {

    /**
     * @brief Result returned from `IdCache::get`.
     */
    typedef struct {
        int32_t id;
        bool    old;
    } CacheResult;

    /**
     * @brief Result returned from `SqlUidCache::get`.
     */
    typedef struct {
        std::vector<unsigned char> uid;
        bool    old;
    } SqlUidCacheResult;

    /**
     * @brief LRU cache that assigns numeric identifiers to frequently used strings.
     *
     * The cache is used for API and error metadata to minimize payload sizes when
     * sending data over gRPC.
     */
    class IdCache {
    public:
        explicit IdCache(const size_t max_size) : max_size_(max_size) {}
        ~IdCache() = default;

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
        void remove(const std::string& key);

    private:
        /**
         * @brief Inserts a new key/id pair while preserving the LRU ordering.
         *
         * @param key String to insert.
         * @param id Newly assigned identifier.
         */
        void put(const std::string& key, int32_t id);

        typedef std::pair<std::string, int32_t> key_id_pair_t;
        std::list<key_id_pair_t> cache_list_{};
        std::unordered_map<std::string, std::list<key_id_pair_t>::iterator> cache_map_{};
        size_t max_size_{};
        std::mutex mutex_{};
        int32_t id_sequence_{0};
    };

    /**
     * @brief LRU cache that assigns binary UIDs to normalized SQL statements.
     */
    class SqlUidCache {
    public:
        explicit SqlUidCache(const size_t max_size) : max_size_(max_size) {}
        ~SqlUidCache() = default;

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
        void remove(const std::string& key);

    private:
        /**
         * @brief Inserts a new SQL UID entry while preserving the LRU ordering.
         *
         * @param key Normalized SQL string.
         * @param uid UID byte vector associated with the key.
         */
        void put(const std::string& key, const std::vector<unsigned char>& uid);

        typedef std::pair<std::string, std::vector<unsigned char>> key_uid_pair_t;
        std::list<key_uid_pair_t> cache_list_{};
        std::unordered_map<std::string, std::list<key_uid_pair_t>::iterator> cache_map_{};
        size_t max_size_{};
        std::mutex mutex_{};
    };

} // namespace pinpoint
