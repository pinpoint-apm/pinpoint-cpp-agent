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

class RateLimiterTest : public ::testing::Test {};

// Basic functionality test - verify allow() returns true when tokens are available
TEST_F(RateLimiterTest, BasicAllowTest) {
    RateLimiter limiter(5); // 5 TPS (5 requests per second)
    
    // First 5 requests should be allowed
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(limiter.allow()) << "Request " << i << " should be allowed";
    }
}

// Rate limiting test - verify false is returned after tokens are exhausted
TEST_F(RateLimiterTest, RateLimitingTest) {
    RateLimiter limiter(3); // 3 TPS
    
    // First 3 requests should be allowed
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(limiter.allow()) << "Request " << i << " should be allowed";
    }
    
    // 4th request should be denied
    EXPECT_FALSE(limiter.allow()) << "4th request should be denied";
    EXPECT_FALSE(limiter.allow()) << "5th request should be denied";
}

// Token bucket refill test - verify tokens are refilled after time passes
TEST_F(RateLimiterTest, TokenRefillTest) {
    RateLimiter limiter(2); // 2 TPS
    
    // Exhaust all tokens
    EXPECT_TRUE(limiter.allow());
    EXPECT_TRUE(limiter.allow());
    EXPECT_FALSE(limiter.allow());
    
    // After waiting 1 second, tokens should be refilled
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    EXPECT_TRUE(limiter.allow()) << "After 1 second, request should be allowed";
    EXPECT_TRUE(limiter.allow()) << "Second request after refill should be allowed";
    EXPECT_FALSE(limiter.allow()) << "Third request should be denied";
}

// 0 TPS test - all requests should be denied
TEST_F(RateLimiterTest, ZeroTpsTest) {
    RateLimiter limiter(0); // 0 TPS
    
    // All requests should be denied
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(limiter.allow()) << "Request " << i << " should be denied with 0 TPS";
    }
}

TEST_F(RateLimiterTest, HighTpsTest) {
    RateLimiter limiter(1000); // 1000 TPS
    
    // All 1000 requests should be allowed
    int allowed_count = 0;
    for (int i = 0; i < 1000; ++i) {
        if (limiter.allow()) {
            allowed_count++;
        }
    }
    
    EXPECT_EQ(allowed_count, 1000) << "All 1000 requests should be allowed";
    
    // 1001st request should be denied
    EXPECT_FALSE(limiter.allow()) << "1001st request should be denied";
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

// Time boundary test - behavior within the same second
TEST_F(RateLimiterTest, SameSecondTest) {
    RateLimiter limiter(3); // 3 TPS
    
    // Make rapid consecutive requests (within the same second)
    bool results[5];
    for (int i = 0; i < 5; ++i) {
        results[i] = limiter.allow();
    }
    
    // First 3 should be true, the rest should be false
    EXPECT_TRUE(results[0]);
    EXPECT_TRUE(results[1]);
    EXPECT_TRUE(results[2]);
    EXPECT_FALSE(results[3]);
    EXPECT_FALSE(results[4]);
}

// ========== Edge Case Tests ==========

// Bucket does NOT accumulate unused tokens across seconds
TEST_F(RateLimiterTest, BucketDoesNotAccumulateTest) {
    RateLimiter limiter(3);

    // Use only 1 token out of 3
    EXPECT_TRUE(limiter.allow());

    // Wait 3 seconds (unused tokens should NOT carry over)
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Should get exactly 3 tokens (not 3 + accumulated)
    int allowed = 0;
    for (int i = 0; i < 10; ++i) {
        if (limiter.allow()) {
            allowed++;
        }
    }
    EXPECT_EQ(allowed, 3) << "After long sleep, bucket should reset to exactly tps (3), not accumulate";
}

// Thread safety: exactly tps tokens consumed when demand exceeds supply
TEST_F(RateLimiterTest, ThreadSafetyExactCountTest) {
    const uint64_t tps = 50;
    RateLimiter limiter(tps);
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
    RateLimiter limiter(2);

    EXPECT_TRUE(limiter.allow());
    EXPECT_TRUE(limiter.allow());

    // Call allow() many more times - all should be false
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(limiter.allow()) << "Request " << i << " after exhaustion should be denied";
    }
}

} // namespace pinpoint
