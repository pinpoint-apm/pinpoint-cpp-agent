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

#include "limiter.h"

namespace pinpoint {

    bool RateLimiter::allow() {
        const uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now().time_since_epoch()).count();

        if (auto passed = now - base_time_; passed > 0) {
            base_time_ = now;
            bucket_ = token_;
        }

        if (bucket_ > 0) {
            --bucket_;
            return true;
        }

        return false;
    }
}
