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
     * @brief Token bucket rate limiter used for sampling throughput limits.
     *
     * Capacity is `tps` tokens and the refill rate is `tps` tokens per second
     * off a monotonic clock, which is what the Java agent gets from Guava's
     * `RateLimiter.create(tps)` (SmoothBursty, one second of burst) and the Go
     * agent from `rate.NewLimiter(rate.Every(time.Second/tps), tps)`. A fixed
     * wall-clock window would instead admit up to 2*tps across a second
     * boundary and tie the window to wall time.
     *
     * The whole bucket is one 64-bit atomic: the theoretical arrival time of
     * the next token, in nanoseconds. Tokens are the distance between that
     * time and now (capped at one second's worth), so refill and consume are a
     * single CAS with no fractional-token state to keep in sync, and a losing
     * CAS retries against the time the winner published instead of refilling
     * twice.
     *
     * That clock starts at construction, so the bucket fills while the limiter
     * is idle and unused: Guava resyncs its stopwatch in `setRate()` and turns
     * everything elapsed since into permits on the first `acquire()`, so an
     * agent that sits idle for a second before its first request admits a
     * whole second of burst rather than a single call.
     */
    class RateLimiter {
    public:
        explicit RateLimiter(uint64_t tps);
        virtual ~RateLimiter() = default;

        /**
         * @brief Consumes a token if one is available.
         *
         * @return `true` when the call is permitted.
         */
        bool allow();

    protected:
        /// @brief Monotonic now in nanoseconds; overridden by tests to inject a fake clock.
        virtual int64_t now_nanos() const;

        /**
         * @brief Restarts the bucket's clock at @p now_ns.
         *
         * For a subclass that injects its own clock: the constructor baselines
         * the bucket on the real one, because virtual dispatch during
         * construction cannot reach the override. Call this with the injected
         * clock's starting value to put the bucket back where the constructor
         * meant it to be — an empty bucket whose fill began now.
         */
        void baseline_at(int64_t now_ns) { next_.store(now_ns, std::memory_order_relaxed); }

    private:
        // Nanoseconds per token. 0 only for tps == 0, which allow() rejects
        // outright: zero tokens per second literally admits nothing, and Guava
        // and Go's rate.Every() will not build such a limiter at all. Note this
        // is NOT the config-level "0 = unlimited" of Sampling.*Throughput and
        // CallstackTraceNewThroughput — the call sites spell unlimited as a
        // null limiter and never construct RateLimiter(0).
        const int64_t interval_;
        const int64_t burst_;       // nanoseconds of tokens a full bucket holds (one second)
        std::atomic<int64_t> next_; // arrival time of the next token; the construction time until first use
    };
}
