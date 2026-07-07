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
     *
     * The window second and the remaining tokens are packed into a single
     * 64-bit atomic, so refill-and-consume is one CAS: there is no
     * reset-in-progress flag for other threads to spin on, and a refill can
     * never race with an in-flight token decrement.
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
        static uint64_t current_second();
        static uint64_t pack(uint64_t second, uint32_t tokens);
        static uint64_t state_second(uint64_t state);
        static uint32_t state_tokens(uint64_t state);

        const uint32_t token_;
        std::atomic<uint64_t> state_;
    };
}
