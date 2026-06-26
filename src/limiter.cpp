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
#include <chrono>

namespace pinpoint {

    RateLimiter::RateLimiter(uint64_t tps)
        : token_(tps),
          epoch_(epoch_state(current_second())),
          bucket_(tps) {
    }

    uint64_t RateLimiter::current_second() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    uint64_t RateLimiter::epoch_state(uint64_t second) {
        return second << 1;
    }

    uint64_t RateLimiter::epoch_second(uint64_t state) {
        return state >> 1;
    }

    bool RateLimiter::is_resetting(uint64_t state) {
        return (state & kResetInProgress) != 0;
    }

    bool RateLimiter::allow() {
        if (token_ == 0) {
            return false;
        }

        const auto now = current_second();
        auto epoch = epoch_.load(std::memory_order_acquire);

        for (;;) {
            if (is_resetting(epoch)) {
                epoch = epoch_.load(std::memory_order_acquire);
                continue;
            }

            if (now <= epoch_second(epoch)) {
                break;
            }

            if (epoch_.compare_exchange_weak(epoch, epoch | kResetInProgress,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                bucket_.store(token_, std::memory_order_relaxed);
                epoch_.store(epoch_state(now), std::memory_order_release);
                break;
            }
        }

        auto bucket = bucket_.load(std::memory_order_relaxed);
        while (bucket > 0) {
            if (bucket_.compare_exchange_weak(bucket, bucket - 1,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
                return true;
            }
        }

        return false;
    }
}
