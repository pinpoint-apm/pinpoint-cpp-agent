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
#include <algorithm>
#include <chrono>

namespace pinpoint {

    namespace {
        constexpr uint64_t kSecondMask = 0xFFFFFFFFull;
    }

    RateLimiter::RateLimiter(uint64_t tps)
        : token_(static_cast<uint32_t>(std::min(tps, kSecondMask))),
          state_(pack(current_second(), token_)) {
    }

    uint64_t RateLimiter::current_second() {
        // Truncated to 32 bits by pack(). steady_clock counts from boot, so a
        // wrap takes ~136 years; window changes are detected by inequality,
        // which stays correct across a wrap.
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    uint64_t RateLimiter::pack(uint64_t second, uint32_t tokens) {
        return ((second & kSecondMask) << 32) | tokens;
    }

    uint64_t RateLimiter::state_second(uint64_t state) {
        return state >> 32;
    }

    uint32_t RateLimiter::state_tokens(uint64_t state) {
        return static_cast<uint32_t>(state);
    }

    bool RateLimiter::allow() {
        if (token_ == 0) {
            return false;
        }

        const auto now = current_second() & kSecondMask;
        // No data is published through state_; it is a self-contained
        // counter, so relaxed ordering is sufficient everywhere.
        auto state = state_.load(std::memory_order_relaxed);

        for (;;) {
            if (state_second(state) != now) {
                // New window: refill and consume one token in a single CAS.
                if (state_.compare_exchange_weak(state, pack(now, token_ - 1),
                                                 std::memory_order_relaxed)) {
                    return true;
                }
                continue;
            }

            const auto tokens = state_tokens(state);
            if (tokens == 0) {
                return false;
            }

            if (state_.compare_exchange_weak(state, pack(now, tokens - 1),
                                             std::memory_order_relaxed)) {
                return true;
            }
        }
    }
}
