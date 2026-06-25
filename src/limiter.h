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
#include <cstdint>

namespace pinpoint {

    /**
     * @brief Fixed-window rate limiter used for sampling throughput limits.
     */
    class RateLimiter {
    public:
        explicit RateLimiter(uint64_t tps);

        /**
         * @brief Consumes a token if available, resetting the bucket once per second.
         *
         * @return `true` when the call is permitted.
         */
        bool allow();

    private:
        static constexpr uint64_t kResetInProgress = 1;

        static uint64_t current_second();
        static uint64_t epoch_state(uint64_t second);
        static uint64_t epoch_second(uint64_t state);
        static bool is_resetting(uint64_t state);

        const uint64_t token_;
        std::atomic<uint64_t> epoch_;
        std::atomic<uint64_t> bucket_;
    };
}
