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
#include <limits>

namespace pinpoint {

    namespace {
        constexpr int64_t kNanosPerSecond = 1000000000;

        // The bucket has not been used yet: the first allow() starts the clock
        // and hands out a single token, the state a freshly created Guava
        // RateLimiter is in.
        constexpr int64_t kUnused = std::numeric_limits<int64_t>::min();

        // Above one token per nanosecond the interval would round to zero, so
        // the rate is clamped there; a limiter that fine is unlimited anyway.
        int64_t clamped_tps(const uint64_t tps) {
            return static_cast<int64_t>(std::min<uint64_t>(tps, kNanosPerSecond));
        }
    }

    RateLimiter::RateLimiter(const uint64_t tps)
        : interval_(tps == 0 ? 0 : kNanosPerSecond / clamped_tps(tps)),
          burst_(interval_ * clamped_tps(tps)),
          next_(kUnused) {
    }

    int64_t RateLimiter::now_nanos() const {
        // steady_clock, not system_clock: the bucket must not refill (or stall)
        // because someone stepped the wall clock.
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool RateLimiter::allow() {
        if (interval_ == 0) {
            return false;
        }

        // No data is published through next_; it is a self-contained counter,
        // so relaxed ordering is sufficient everywhere.
        auto next = next_.load(std::memory_order_relaxed);

        for (;;) {
            // Re-read the clock on every attempt: a failed CAS means another
            // thread consumed a token, possibly a long time ago.
            const auto now = now_nanos();

            // Refill. The tokens on hand are (now - next) / interval_, so
            // moving next_ no further back than now - burst_ caps an idle
            // bucket at exactly tps tokens - the refill cannot be applied
            // twice because the CAS below publishes the consumed state from
            // the same value it was computed on.
            const auto refilled = next == kUnused ? now : std::max(next, now - burst_);
            if (refilled > now) {
                // The next token is not due yet.
                return false;
            }

            if (next_.compare_exchange_weak(next, refilled + interval_,
                                            std::memory_order_relaxed)) {
                return true;
            }
        }
    }
}
