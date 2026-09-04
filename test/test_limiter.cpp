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

#include "../src/limiter.h"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <vector>
#include <future>

namespace pinpoint {

// Fake clock: the bucket only ever reads time through now_nanos(), so the
// refill schedule can be tested without sleeping.
class FakeClockLimiter final : public RateLimiter {
public:
    // baseline_at re-does what the base constructor could not: it read the
    // real clock, because virtual dispatch during construction does not reach
    // the override below. Zero is this clock's construction time.
    explicit FakeClockLimiter(const uint64_t tps) : RateLimiter(tps) { baseline_at(now_ns_); }

    void advance_ms(const int64_t ms) { now_ns_ += ms * 1000000; }
    int64_t elapsed_ms() const { return now_ns_ / 1000000; }

protected:
    int64_t now_nanos() const override { return now_ns_; }

private:
    int64_t now_ns_{0};
};

class RateLimiterTest : public ::testing::Test {};

// Called at once, a fresh bucket is empty: the first call goes through and the
// next one has to wait a full token interval, exactly like a freshly created
// Guava RateLimiter (which also lets the first caller borrow against the
// future). What it accumulates while nobody calls it is the test below.
TEST_F(RateLimiterTest, FirstCallPassesThenPacesAtTps) {
    FakeClockLimiter limiter(10); // one token per 100ms

    EXPECT_TRUE(limiter.allow()) << "first call should pass";
    EXPECT_FALSE(limiter.allow()) << "no token is due yet";

    limiter.advance_ms(50);
    EXPECT_FALSE(limiter.allow()) << "half an interval is not a token";

    limiter.advance_ms(50);
    EXPECT_TRUE(limiter.allow()) << "one interval elapsed, one token due";
    EXPECT_FALSE(limiter.allow());
}

// Regression: the bucket fills from construction, not from the first call.
// Guava starts its stopwatch in setRate() and converts everything elapsed
// since into permits on the first acquire(), so an agent that idles before its
// first request owes that request a full second of burst. Treating "never
// used" as "no time has passed" handed out exactly one, which under-sampled
// the very first traffic a process saw.
TEST_F(RateLimiterTest, IdleBeforeTheFirstCallStillFillsTheBucket) {
    FakeClockLimiter limiter(10);  // one token per 100ms

    limiter.advance_ms(1000);  // a second of idle, with no call yet

    int allowed = 0;
    for (int i = 0; i < 50; ++i) {
        if (limiter.allow()) {
            ++allowed;
        }
    }

    // tps stored tokens, plus the one Guava lets through when the bucket is
    // empty but the clock has reached the next token — the same +1 the
    // long-idle test below documents.
    EXPECT_EQ(allowed, 11) << "an idle second before the first call is still a second of tokens";
    EXPECT_FALSE(limiter.allow());
}

// The same property on the real clock — the path production actually takes,
// and the only one that exercises the constructor's own clock read (a fake
// clock has to re-baseline, so it cannot tell where the baseline came from).
// Bounds are wide: what matters is that idle time was worth many tokens
// rather than the single one an empty bucket always lets through.
TEST_F(RateLimiterTest, RealClockIdleBeforeTheFirstCallFillsTheBucket) {
    RateLimiter limiter(100);  // one token per 10ms

    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    int allowed = 0;
    for (int i = 0; i < 200; ++i) {
        if (limiter.allow()) {
            ++allowed;
        }
    }

    EXPECT_GE(allowed, 10) << "250ms idle before the first call is worth ~25 tokens";
    EXPECT_LE(allowed, 100) << "and never more than the one-second cap";
}

// Acceptance criteria: at tps=10, called every 50ms, the first second admits at
// most 10 and no 200ms window around a second boundary admits more than 11.
// A fixed wall-clock window would admit up to 2 * tps across the boundary.
TEST_F(RateLimiterTest, SteadyCallsNeverExceedTps) {
    FakeClockLimiter limiter(10);

    int first_second = 0;
    int around_boundary = 0;
    int total = 0;

    for (int i = 0; i < 100; ++i) { // 100 calls x 50ms = 5 seconds
        const auto at_ms = limiter.elapsed_ms();
        if (limiter.allow()) {
            ++total;
            if (at_ms < 1000) {
                ++first_second;
            }
            if (at_ms >= 900 && at_ms <= 1100) {
                ++around_boundary;
            }
        }
        limiter.advance_ms(50);
    }

    EXPECT_LE(first_second, 10) << "first second must not exceed tps";
    EXPECT_LE(around_boundary, 11) << "no burst at the second boundary";
    EXPECT_EQ(total, 50) << "5 seconds at 10 tps";
}

// After an idle second the bucket holds one second of tokens, not more.
TEST_F(RateLimiterTest, IdleBurstIsCappedAtTps) {
    FakeClockLimiter limiter(10);

    EXPECT_TRUE(limiter.allow());
    limiter.advance_ms(1000);

    int allowed = 0;
    for (int i = 0; i < 50; ++i) {
        if (limiter.allow()) {
            ++allowed;
        }
    }

    EXPECT_EQ(allowed, 10) << "one idle second must not release more than tps";
    EXPECT_FALSE(limiter.allow());
}

// Idling for longer does not accumulate: the bucket stops at tps stored tokens,
// plus the one call Guava lets through when the bucket is empty but the clock
// has caught up with the next token.
TEST_F(RateLimiterTest, LongIdleDoesNotAccumulate) {
    FakeClockLimiter limiter(3);

    EXPECT_TRUE(limiter.allow());
    limiter.advance_ms(5000);

    int allowed = 0;
    for (int i = 0; i < 20; ++i) {
        if (limiter.allow()) {
            ++allowed;
        }
    }

    EXPECT_EQ(allowed, 4) << "5 idle seconds still cap the bucket at tps (+1 due token)";
}

// 0 TPS test - all requests should be denied
TEST_F(RateLimiterTest, ZeroTpsTest) {
    RateLimiter limiter(0); // 0 TPS
    
    // All requests should be denied
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(limiter.allow()) << "Request " << i << " should be denied with 0 TPS";
    }
}

// The default clock path (steady_clock) refills too.
TEST_F(RateLimiterTest, RealClockRefillTest) {
    RateLimiter limiter(10); // one token per 100ms

    EXPECT_TRUE(limiter.allow());
    EXPECT_FALSE(limiter.allow());

    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    int allowed = 0;
    for (int i = 0; i < 10; ++i) {
        if (limiter.allow()) {
            allowed++;
        }
    }

    EXPECT_GE(allowed, 1) << "tokens should refill on the real clock";
    EXPECT_LE(allowed, 4) << "250ms must not release a full second of tokens";
}

TEST_F(RateLimiterTest, ThreadSafetyTest) {
    RateLimiter limiter(100); // 100 TPS
    const int num_threads = 10;
    const int requests_per_thread = 20;
    
    std::vector<std::future<int>> futures;
    
    // Make requests from multiple threads simultaneously
    for (int i = 0; i < num_threads; ++i) {
        futures.push_back(std::async(std::launch::async, [&limiter]() {
            int allowed = 0;
            for (int j = 0; j < requests_per_thread; ++j) {
                if (limiter.allow()) {
                    allowed++;
                }
            }
            return allowed;
        }));
    }
    
    // Wait for all threads to complete and collect results
    int total_allowed = 0;
    for (auto& future : futures) {
        total_allowed += future.get();
    }
    
    // Total allowed requests should not exceed 100
    EXPECT_LE(total_allowed, 100) << "Total allowed requests should not exceed the rate limit";
    EXPECT_GT(total_allowed, 0) << "At least some requests should be allowed";
}

// ========== Edge Case Tests ==========

// Thread safety: with the clock frozen on a full bucket, concurrent callers
// consume exactly the tokens on hand - no lost update, no double refill.
TEST_F(RateLimiterTest, ThreadSafetyExactCountTest) {
    const uint64_t tps = 50;
    FakeClockLimiter limiter(tps);

    EXPECT_TRUE(limiter.allow());
    limiter.advance_ms(1000); // bucket refilled to its cap, clock frozen from here

    const int num_threads = 20;
    const int requests_per_thread = 10; // 200 total requests > 50 tps

    std::vector<std::future<int>> futures;

    for (int i = 0; i < num_threads; ++i) {
        futures.push_back(std::async(std::launch::async, [&limiter]() {
            int allowed = 0;
            for (int j = 0; j < requests_per_thread; ++j) {
                if (limiter.allow()) {
                    allowed++;
                }
            }
            return allowed;
        }));
    }

    int total_allowed = 0;
    for (auto& future : futures) {
        total_allowed += future.get();
    }

    EXPECT_EQ(total_allowed, static_cast<int>(tps))
        << "Exactly tps tokens should be allowed across all threads";
}

// Repeated deny after exhaustion (calling allow many times after bucket is empty)
TEST_F(RateLimiterTest, RepeatedDenyAfterExhaustionTest) {
    FakeClockLimiter limiter(2);

    EXPECT_TRUE(limiter.allow());

    // Call allow() many more times - all should be false
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(limiter.allow()) << "Request " << i << " after exhaustion should be denied";
    }
}

} // namespace pinpoint
