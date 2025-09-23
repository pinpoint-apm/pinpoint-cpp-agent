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

class RateLimiterTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Called before each test
    }

    void TearDown() override {
        // Called after each test
    }
};

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

// Multiple refill test
TEST_F(RateLimiterTest, MultipleRefillTest) {
    RateLimiter limiter(1); // 1 TPS
    
    for (int cycle = 0; cycle < 3; ++cycle) {
        EXPECT_TRUE(limiter.allow()) << "Cycle " << cycle << " first request should be allowed";
        EXPECT_FALSE(limiter.allow()) << "Cycle " << cycle << " second request should be denied";
        
        // Wait 1 second for next cycle
        if (cycle < 2) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

// 0 TPS test - all requests should be denied
TEST_F(RateLimiterTest, ZeroTpsTest) {
    RateLimiter limiter(0); // 0 TPS
    
    // All requests should be denied
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(limiter.allow()) << "Request " << i << " should be denied with 0 TPS";
    }
}

// High TPS test
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

// Thread safety test
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

// Refill test after long interval
TEST_F(RateLimiterTest, LongIntervalRefillTest) {
    RateLimiter limiter(5); // 5 TPS
    
    // Exhaust all tokens
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(limiter.allow());
    }
    EXPECT_FALSE(limiter.allow());
    
    // Wait 2 seconds (verify correct refill after multiple cycles)
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Tokens should be refilled (but maximum is 5)
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(limiter.allow()) << "Request " << i << " should be allowed after long interval";
    }
    EXPECT_FALSE(limiter.allow()) << "6th request should be denied even after long interval";
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

} // namespace pinpoint
