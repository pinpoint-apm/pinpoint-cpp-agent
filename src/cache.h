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

namespace pinpoint {

    typedef struct {
        int32_t id;
        bool    old;
    } CacheResult;

    class IdCache {
    public:
        explicit IdCache(const size_t max_size) : max_size_(max_size) {}
        ~IdCache() = default;

        CacheResult get(const std::string& key);
        void remove(const std::string& key);

    private:
        void put(const std::string& key, int32_t id);

        typedef std::pair<std::string, int32_t> key_id_pair_t;
        std::list<key_id_pair_t> cache_list_{};
        std::unordered_map<std::string, std::list<key_id_pair_t>::iterator> cache_map_{};
        size_t max_size_{};
        std::mutex mutex_{};
        int32_t id_sequence_{0};
    };

} // namespace pinpoint
