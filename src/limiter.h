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
#include <chrono>

namespace pinpoint {

    class RateLimiter {
    public:
        explicit RateLimiter(const uint64_t tps) {
            token_ = bucket_ = tps;
            base_time_ = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool allow();

    private:
        uint64_t token_ = 0;
        std::atomic<uint64_t> base_time_ = {0};
        std::atomic<uint64_t> bucket_ = {0};
    };
}
