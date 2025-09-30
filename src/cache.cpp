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

#include "cache.h"
#include "utility.h"

namespace pinpoint {

    CacheResult IdCache::get(const std::string& key) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (const auto it = cache_map_.find(key); it != cache_map_.end()) {
            cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
            return CacheResult{it->second->second, true};
        }

        const auto new_id = ++id_sequence_;
        put(key, new_id);
        return CacheResult{new_id, false};
    }

    void IdCache::remove(const std::string& key) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (const auto it = cache_map_.find(key); it != cache_map_.end()) {
            cache_list_.erase(it->second);
            cache_map_.erase(it);
        }
    }

    void IdCache::put(const std::string& key, int32_t id) {
        cache_list_.emplace_front(key, id);
        cache_map_[key] = cache_list_.begin();

        if (cache_map_.size() > max_size_) {
            auto victim = cache_list_.end();
            --victim;
            cache_map_.erase(victim->first);
            cache_list_.pop_back();
        }
    }

    SqlUidCacheResult SqlUidCache::get(const std::string& key) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (const auto it = cache_map_.find(key); it != cache_map_.end()) {
            cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
            return SqlUidCacheResult{it->second->second, true};
        }

        const auto new_uid = generate_sql_uid(key);
        put(key, new_uid);
        return SqlUidCacheResult{new_uid, false};
    }

    void SqlUidCache::remove(const std::string& key) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (const auto it = cache_map_.find(key); it != cache_map_.end()) {
            cache_list_.erase(it->second);
            cache_map_.erase(it);
        }
    }

    void SqlUidCache::put(const std::string& key, const std::vector<unsigned char>& uid) {
        cache_list_.emplace_front(key, uid);
        cache_map_[key] = cache_list_.begin();

        if (cache_map_.size() > max_size_) {
            auto victim = cache_list_.end();
            --victim;
            cache_map_.erase(victim->first);
            cache_list_.pop_back();
        }
    }
} // namespace pinpoint
