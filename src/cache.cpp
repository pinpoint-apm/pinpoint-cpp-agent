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
        // Use the template cache with a lambda generator for new IDs
        return cache_.get(key, [this]() {
            return ++id_sequence_;
        });
    }

    SqlUidCacheResult SqlUidCache::get(const std::string& key) {
        // Use the template cache with a lambda generator for new UIDs
        return cache_.get(key, [&key]() {
            return generate_sql_uid(key);
        });
    }
} // namespace pinpoint
