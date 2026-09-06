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

#include "../src/sampling.h"
#include "../src/stat.h"
#include "../src/url_stat.h"
#include "../src/agent_service.h"
#include "../src/config.h"
#include "mock_agent_service.h"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <vector>
#include <future>

namespace pinpoint {

class SamplingTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_service_ = std::make_unique<MockAgentService>();
    }

    void TearDown() override {
        mock_service_.reset();
    }
    
    std::unique_ptr<MockAgentService> mock_service_;
};

// CounterSampler Tests

// Test CounterSampler with rate 0 - should always return false
TEST_F(SamplingTest, CounterSamplerZeroRateTest) {
    CounterSampler sampler(0);
    
    // All calls should return false with rate 0
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(sampler.isSampled()) << "Call " << i << " should return false with rate 0";
    }
}

// Test CounterSampler with rate 1 - should always return true
TEST_F(SamplingTest, CounterSamplerOneRateTest) {
    CounterSampler sampler(1);
    
    // All calls should return true with rate 1
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(sampler.isSampled()) << "Call " << i << " should return true with rate 1";
    }
}

// Test CounterSampler with rate N - should return true on the first call of
// every N-call cycle (Java CountingSampler phase: counter starts at 0)
TEST_F(SamplingTest, CounterSamplerNRateTest) {
    const int rate = 3;
    CounterSampler sampler(rate);

    // Test pattern: true, false, false, true, false, false, ...
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (int i = 0; i < rate; ++i) {
            bool expected = (i == 0);
            EXPECT_EQ(sampler.isSampled(), expected)
                << "Cycle " << cycle << ", call " << i << " should return " << expected;
        }
    }
}

// The first request must be sampled: the counter is tested before it is
// incremented, so rate=10 samples requests 1, 11, 21, ... like Java's
// CountingSampler, not 10, 20, 30 (which hides the first request entirely
// on low-traffic deployments and in tests).
TEST_F(SamplingTest, CounterSamplerSamplesFirstRequestTest) {
    const int rate = 10;
    CounterSampler sampler(rate);

    EXPECT_TRUE(sampler.isSampled()) << "Request 1 should be sampled";
    for (int i = 2; i <= rate; ++i) {
        EXPECT_FALSE(sampler.isSampled()) << "Request " << i << " should be unsampled";
    }
    EXPECT_TRUE(sampler.isSampled()) << "Request " << (rate + 1) << " should be sampled";
}

TEST_F(SamplingTest, CounterSamplerThreadSafetyTest) {
    const int rate = 10;
    const int num_threads = 5;
    const int calls_per_thread = 20;
    CounterSampler sampler(rate);
    
    std::vector<std::future<int>> futures;
    
    // Launch multiple threads
    for (int i = 0; i < num_threads; ++i) {
        futures.push_back(std::async(std::launch::async, [&sampler]() {
            int true_count = 0;
            for (int j = 0; j < calls_per_thread; ++j) {
                if (sampler.isSampled()) {
                    true_count++;
                }
            }
            return true_count;
        }));
    }
    
    int total_true_count = 0;
    for (auto& future : futures) {
        total_true_count += future.get();
    }
    
    // Total calls / rate should equal true count
    int total_calls = num_threads * calls_per_thread;
    int expected_true_count = total_calls / rate;
    EXPECT_EQ(total_true_count, expected_true_count) 
        << "Expected " << expected_true_count << " true results from " << total_calls << " calls";
}

// PercentSampler Tests

// Test PercentSampler with rate 0.0 - should always return false
TEST_F(SamplingTest, PercentSamplerZeroRateTest) {
    PercentSampler sampler(0.0);
    
    // All calls should return false with rate 0.0
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(sampler.isSampled()) << "Call " << i << " should return false with rate 0.0";
    }
}

// The first request must be sampled, same as CounterSampler and Java's
// PercentRateSampler: the admission window is (0, rate_], so 50% samples
// requests 1, 3, 5, ... A [0, rate_) window keeps the frequency but shifts the
// phase by one, hiding the first request on low-traffic deployments and in tests.
TEST_F(SamplingTest, PercentSamplerSamplesFirstRequestTest) {
    PercentSampler half(50.0);
    for (int i = 1; i <= 6; ++i) {
        const bool expected = (i % 2) == 1;
        EXPECT_EQ(half.isSampled(), expected)
            << "Request " << i << " at 50% should be " << (expected ? "sampled" : "unsampled");
    }

    // Same at a rate that only admits once per cycle: request 1, then 101, ...
    PercentSampler one_percent(1.0);
    EXPECT_TRUE(one_percent.isSampled()) << "Request 1 at 1% should be sampled";
    for (int i = 2; i <= 100; ++i) {
        EXPECT_FALSE(one_percent.isSampled()) << "Request " << i << " at 1% should be unsampled";
    }
    EXPECT_TRUE(one_percent.isSampled()) << "Request 101 at 1% should be sampled";
}

// Out-of-range rates are clamped in the constructor: a negative rate behaves
// as never-sample (like CounterSampler's <= 0 guard) and a rate above 100%
// behaves as always-sample, even without the config-layer validation.
TEST_F(SamplingTest, PercentSamplerOutOfRangeRateClampTest) {
    PercentSampler negative_sampler(-5.0);
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(negative_sampler.isSampled())
            << "Call " << i << " should return false with a negative rate";
    }

    PercentSampler over_sampler(250.0);
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(over_sampler.isSampled())
            << "Call " << i << " should return true with a rate above 100%";
    }
}

// Cross-agent parity: 0.29% must truncate to 28/10000, like Java's
// PercentSamplerFactory (`(long) (samplingRateDouble * MULTIPLIER)`) and Go's
// `uint64(percent * 100)`, not round to 29 — 0.29 * 100 is 28.999999999999996
// in double. The percent sampler is deterministic and the counter advances by
// rate_ per call, so over exactly MAX_PERCENT_RATE calls it wraps a whole
// number of times and samples true exactly rate_ times.
TEST_F(SamplingTest, PercentSamplerTruncatesFractionalRateTest) {
    PercentSampler sampler(0.29);

    int true_count = 0;
    for (int i = 0; i < MAX_PERCENT_RATE; ++i) {
        if (sampler.isSampled()) {
            true_count++;
        }
    }

    EXPECT_EQ(true_count, 28)
        << "0.29% should sample exactly 28 per " << MAX_PERCENT_RATE
        << " (truncated, as Java and Go do), not 29 (rounded)";
}

// Truncation disables every rate below one hundredth-of-a-percent — including
// 0.006, which the previous rounding kept alive at 1/MAX_PERCENT_RATE. The
// config passes such a rate straight through (it warns, it does not raise it),
// so this is a reachable configuration and it means never-sample, exactly as
// Java's parseSamplingRate + createSampler resolve it
// (PercentSamplerFactory.java:40-48,56-58). 0.01 * 100 is exactly 1.0, so the
// smallest rate that survives truncation still samples.
TEST_F(SamplingTest, PercentSamplerTruncatesSubHundredthRateToDisabledTest) {
    for (const double rate : {0.004, 0.006, 0.009}) {
        PercentSampler truncates_to_zero(rate);
        for (int i = 0; i < 200; ++i) {
            ASSERT_FALSE(truncates_to_zero.isSampled())
                << "Rate " << rate << " call " << i
                << ": a rate truncating to 0 must never sample";
        }
    }

    // 0.01% is the smallest rate that survives truncation and stays enabled,
    // sampling exactly once per MAX_PERCENT_RATE. Removing the config floor must
    // not break it.
    PercentSampler smallest_enabled(0.01);
    int true_count = 0;
    for (int i = 0; i < MAX_PERCENT_RATE; ++i) {
        if (smallest_enabled.isSampled()) {
            true_count++;
        }
    }
    EXPECT_EQ(true_count, 1)
        << "0.01% (the config minimum) should stay enabled at 1/" << MAX_PERCENT_RATE;
}

// The upper edge truncates too: 99.999 * 100 is 9999.9, so rate_ is 9999 and the
// sampler misses exactly one call per MAX_PERCENT_RATE instead of short-circuiting
// to always-sample. Java agrees — PercentSamplerFactory hands off to TrueSampler
// only at MAX_PERCENT_RATE, which 99.999 no longer reaches once truncated. An
// exact 100 still does.
TEST_F(SamplingTest, PercentSamplerTruncatesNearFullRateTest) {
    PercentSampler near_full(99.999);
    int true_count = 0;
    for (int i = 0; i < MAX_PERCENT_RATE; ++i) {
        if (near_full.isSampled()) {
            true_count++;
        }
    }
    EXPECT_EQ(true_count, MAX_PERCENT_RATE - 1)
        << "99.999% truncates to 9999/" << MAX_PERCENT_RATE << ", not always-sample";

    PercentSampler full(100.0);
    for (int i = 0; i < MAX_PERCENT_RATE; ++i) {
        ASSERT_TRUE(full.isSampled())
            << "Call " << i << ": 100% must be deterministic always-sample";
    }
}

TEST_F(SamplingTest, PercentSamplerVariousRatesTest) {
    // In this implementation, rate is percentage (1.0 = 1%, 5.0 = 5%, etc.)
    const std::vector<std::pair<double, double>> test_cases = {
        {1.0, 0.01},   // 1.0 input = 1% sampling
        {5.0, 0.05},   // 5.0 input = 5% sampling
        {10.0, 0.10},  // 10.0 input = 10% sampling
        {20.0, 0.20}   // 20.0 input = 20% sampling
    };
    const int total_calls = 100000;
    
    for (auto& test_case : test_cases) {
        double input_rate = test_case.first;
        double expected_rate = test_case.second;
        
        PercentSampler sampler(input_rate);
        int true_count = 0;
        
        for (int i = 0; i < total_calls; ++i) {
            if (sampler.isSampled()) {
                true_count++;
            }
        }
        
        double actual_rate = static_cast<double>(true_count) / total_calls;
        double tolerance = std::max(0.005, expected_rate * 0.1); // Adaptive tolerance
        
        EXPECT_NEAR(actual_rate, expected_rate, tolerance)
            << "Input rate " << input_rate << " (expected " << expected_rate 
            << ") resulted in actual rate " << actual_rate;
    }
}

TEST_F(SamplingTest, PercentSamplerThreadSafetyTest) {
    const double input_rate = 5.0; // 5.0 input = 5% sampling
    const double expected_rate = 0.05;
    const int num_threads = 4;
    const int calls_per_thread = 10000;
    PercentSampler sampler(input_rate);
    
    std::vector<std::future<int>> futures;
    
    // Launch multiple threads
    for (int i = 0; i < num_threads; ++i) {
        futures.push_back(std::async(std::launch::async, [&sampler]() {
            int true_count = 0;
            for (int j = 0; j < calls_per_thread; ++j) {
                if (sampler.isSampled()) {
                    true_count++;
                }
            }
            return true_count;
        }));
    }
    
    int total_true_count = 0;
    for (auto& future : futures) {
        total_true_count += future.get();
    }
    
    int total_calls = num_threads * calls_per_thread;
    double actual_rate = static_cast<double>(total_true_count) / total_calls;
    double tolerance = std::max(0.01, expected_rate * 0.3); // More lenient tolerance for threading
    
    EXPECT_NEAR(actual_rate, expected_rate, tolerance)
        << "Expected rate " << expected_rate << " but got " << actual_rate;
}

// Pass-through (no-limiter) trace sampler tests: with both throughputs 0,
// TraceSampler delegates straight to the underlying Sampler.

// Test the pass-through sampler with a null sampler
TEST_F(SamplingTest, PassThroughTraceSamplerNullSamplerTest) {
    TraceSampler trace_sampler(mock_service_.get(), nullptr, 0, 0);

    // Should always return false for new samples when sampler is null
    EXPECT_FALSE(trace_sampler.isNewSampled());
    EXPECT_FALSE(trace_sampler.isNewSampled());

    // Should always return true for continue samples
    EXPECT_TRUE(trace_sampler.isContinueSampled());
    EXPECT_TRUE(trace_sampler.isContinueSampled());
}

// Test the pass-through sampler with CounterSampler
TEST_F(SamplingTest, PassThroughTraceSamplerWithCounterSamplerTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(2);
    TraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), 0, 0);

    // Test pattern: true, false, true, false, ...
    EXPECT_TRUE(trace_sampler.isNewSampled());
    EXPECT_FALSE(trace_sampler.isNewSampled());
    EXPECT_TRUE(trace_sampler.isNewSampled());
    EXPECT_FALSE(trace_sampler.isNewSampled());

    // Continue samples should always be true
    EXPECT_TRUE(trace_sampler.isContinueSampled());
    EXPECT_TRUE(trace_sampler.isContinueSampled());
}

// Test the pass-through sampler with PercentSampler
TEST_F(SamplingTest, PassThroughTraceSamplerWithPercentSamplerTest) {
    auto percent_sampler = std::make_unique<PercentSampler>(10.0); // 10.0 input = 10% sampling
    TraceSampler trace_sampler(mock_service_.get(), std::move(percent_sampler), 0, 0);
    
    // Test over many calls to check approximate rate
    int true_count = 0;
    int total_calls = 10000;
    
    for (int i = 0; i < total_calls; ++i) {
        if (trace_sampler.isNewSampled()) {
            true_count++;
        }
    }
    
    double actual_rate = static_cast<double>(true_count) / total_calls;
    double expected_rate = 0.10; // 10%
    double tolerance = 0.02; // 2% tolerance
    
    EXPECT_NEAR(actual_rate, expected_rate, tolerance)
        << "Expected rate " << expected_rate << " but got " << actual_rate;
    
    // Continue samples should always be true
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(trace_sampler.isContinueSampled()) << "Continue call " << i << " should be sampled";
    }
}

// TraceSampler Tests

TEST_F(SamplingTest, TraceSamplerNoLimitersTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1); // 100% sampling
    TraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), 0, 0); // No limiters
    
    // Should follow sampler behavior without rate limiting
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(trace_sampler.isNewSampled()) << "New call " << i << " should be sampled";
        EXPECT_TRUE(trace_sampler.isContinueSampled()) << "Continue call " << i << " should be sampled";
    }
}

// Test TraceSampler with new_tps limiter
TEST_F(SamplingTest, TraceSamplerNewLimiterTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1); // 100% sampling
    const int new_tps = 3;
    TraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), new_tps, 0);
    
    // The bucket is used the instant it is built, so it has had no idle time
    // to fill (see RateLimiter): back-to-back calls get the one token a due
    // bucket lends out and then wait 1/new_tps of a second for the next.
    EXPECT_TRUE(trace_sampler.isNewSampled()) << "First new call should be allowed";

    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(trace_sampler.isNewSampled()) << "New call " << (i + 1) << " should be blocked";
    }
    
    // Continue samples should not be limited (no continue limiter)
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(trace_sampler.isContinueSampled()) << "Continue call " << i << " should be sampled";
    }
}

// Test TraceSampler with continue_tps limiter
TEST_F(SamplingTest, TraceSamplerContinueLimiterTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1); // 100% sampling
    const int continue_tps = 2;
    TraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), 0, continue_tps);
    
    // New samples should not be limited (no new limiter)
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(trace_sampler.isNewSampled()) << "New call " << i << " should be sampled";
    }
    
    // One token from the fresh bucket, then the refill interval applies.
    EXPECT_TRUE(trace_sampler.isContinueSampled()) << "First continue call should be allowed";

    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(trace_sampler.isContinueSampled()) << "Continue call " << (i + 1) << " should be blocked";
    }
}

TEST_F(SamplingTest, TraceSamplerBothLimitersTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1); // 100% sampling
    const int new_tps = 2;
    const int continue_tps = 3;
    TraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), new_tps, continue_tps);
    
    // Each limiter hands out one token from its fresh bucket, then paces.
    EXPECT_TRUE(trace_sampler.isNewSampled()) << "First new call should be allowed";
    EXPECT_FALSE(trace_sampler.isNewSampled()) << "Additional new call should be blocked";

    EXPECT_TRUE(trace_sampler.isContinueSampled()) << "First continue call should be allowed";
    EXPECT_FALSE(trace_sampler.isContinueSampled()) << "Additional continue call should be blocked";
}

// Test TraceSampler with sampler that blocks
TEST_F(SamplingTest, TraceSamplerWithBlockingSamplerTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(0); // 0% sampling (always blocks)
    TraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), 10, 10);
    
    // Should be blocked by sampler before reaching rate limiter
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(trace_sampler.isNewSampled()) << "New call " << i << " should be blocked by sampler";
    }
    
    // Continue samples reach the rate limiter, which allows the one token its
    // fresh bucket holds.
    EXPECT_TRUE(trace_sampler.isContinueSampled()) << "First continue call should be allowed";
    EXPECT_FALSE(trace_sampler.isContinueSampled()) << "Additional continue call should be blocked by rate limiter";
}

// Edge Case Tests

// Test CounterSampler with negative rate - should be treated as disabled.
// Without the rate_ <= 0 guard, `count % rate_` would convert the negative
// rate_ to a huge uint64 and silently "almost never" sample instead.
TEST_F(SamplingTest, CounterSamplerNegativeRateTest) {
    CounterSampler sampler(-1);

    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(sampler.isSampled()) << "Call " << i << " should return false with a negative rate";
    }
}

// Test CounterSampler with very large rate
TEST_F(SamplingTest, CounterSamplerLargeRateTest) {
    const int rate = 1000000;
    CounterSampler sampler(rate);

    // The first call is sampled, the following (rate - 1) are not
    EXPECT_TRUE(sampler.isSampled()) << "First call should return true for large rate";
    for (int i = 1; i < 10; ++i) {
        EXPECT_FALSE(sampler.isSampled()) << "Call " << i << " should return false for large rate";
    }
}

// AgentStats Counter Verification Tests

// Test that the pass-through sampler correctly increments sample_new stats
TEST_F(SamplingTest, PassThroughTraceSamplerStatsIncrementNewSampledTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1); // always sampled
    TraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), 0, 0);
    auto& stats = mock_service_->getAgentStats();

    for (int i = 0; i < 5; ++i) {
        trace_sampler.isNewSampled();
    }

    // Collect stats snapshot to verify counters
    AgentStatsSnapshot snapshot;
    stats.collectAgentStat(snapshot);
    EXPECT_EQ(snapshot.num_sample_new_, 5) << "Should have 5 sampled new traces";
    EXPECT_EQ(snapshot.num_unsample_new_, 0) << "Should have 0 unsampled new traces";
}

// Test that the pass-through sampler correctly increments unsample_new stats
TEST_F(SamplingTest, PassThroughTraceSamplerStatsIncrementNewUnsampledTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(0); // never sampled
    TraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), 0, 0);
    auto& stats = mock_service_->getAgentStats();

    for (int i = 0; i < 5; ++i) {
        trace_sampler.isNewSampled();
    }

    AgentStatsSnapshot snapshot;
    stats.collectAgentStat(snapshot);
    EXPECT_EQ(snapshot.num_sample_new_, 0) << "Should have 0 sampled new traces";
    EXPECT_EQ(snapshot.num_unsample_new_, 5) << "Should have 5 unsampled new traces";
}

// Test that the pass-through sampler correctly increments sample_cont stats
TEST_F(SamplingTest, PassThroughTraceSamplerStatsIncrementContTest) {
    TraceSampler trace_sampler(mock_service_.get(), nullptr, 0, 0);
    auto& stats = mock_service_->getAgentStats();

    for (int i = 0; i < 7; ++i) {
        trace_sampler.isContinueSampled();
    }

    AgentStatsSnapshot snapshot;
    stats.collectAgentStat(snapshot);
    EXPECT_EQ(snapshot.num_sample_cont_, 7) << "Should have 7 sampled continue traces";
}

// Test TraceSampler skip_new stats when rate limited
TEST_F(SamplingTest, TraceSamplerStatsSkipNewTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1); // always sampled
    const int new_tps = 2;
    TraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), new_tps, 0);
    auto& stats = mock_service_->getAgentStats();

    // One token from the fresh bucket passes, the next 4 get rate limited
    // (skipped) - the bucket refills at new_tps/s, far slower than this loop.
    for (int i = 0; i < 5; ++i) {
        trace_sampler.isNewSampled();
    }

    AgentStatsSnapshot snapshot;
    stats.collectAgentStat(snapshot);
    EXPECT_EQ(snapshot.num_sample_new_, 1) << "Should have 1 sampled new trace";
    EXPECT_EQ(snapshot.num_skip_new_, 4) << "Should have 4 skipped new traces";
    EXPECT_EQ(snapshot.num_unsample_new_, 0) << "Should have 0 unsampled new traces";
}

// Test TraceSampler skip_cont stats when rate limited
TEST_F(SamplingTest, TraceSamplerStatsSkipContTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1);
    const int continue_tps = 3;
    TraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), 0, continue_tps);
    auto& stats = mock_service_->getAgentStats();

    // One token from the fresh bucket passes, the next 6 get rate limited.
    for (int i = 0; i < 7; ++i) {
        trace_sampler.isContinueSampled();
    }

    AgentStatsSnapshot snapshot;
    stats.collectAgentStat(snapshot);
    EXPECT_EQ(snapshot.num_sample_cont_, 1) << "Should have 1 sampled continue trace";
    EXPECT_EQ(snapshot.num_skip_cont_, 6) << "Should have 6 skipped continue traces";
}

// Test TraceSampler unsample_new stats when sampler rejects
TEST_F(SamplingTest, TraceSamplerStatsUnsampleNewTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(0); // never sampled
    TraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), 10, 10);
    auto& stats = mock_service_->getAgentStats();

    for (int i = 0; i < 5; ++i) {
        trace_sampler.isNewSampled();
    }

    AgentStatsSnapshot snapshot;
    stats.collectAgentStat(snapshot);
    EXPECT_EQ(snapshot.num_unsample_new_, 5) << "Should have 5 unsampled (sampler rejected)";
    EXPECT_EQ(snapshot.num_sample_new_, 0) << "Should have 0 sampled";
    EXPECT_EQ(snapshot.num_skip_new_, 0) << "Should have 0 skipped (rate limiter never reached)";
}

TEST_F(SamplingTest, TraceSamplerNullSamplerTest) {
    TraceSampler trace_sampler(mock_service_.get(), nullptr, 5, 5);

    // Null sampler should always return false for new samples
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(trace_sampler.isNewSampled()) << "Null sampler should reject new sample " << i;
    }

    // Continue samples still reach the rate limiter, which hands out the one
    // token its fresh bucket holds.
    EXPECT_TRUE(trace_sampler.isContinueSampled()) << "First continue call should be allowed";
    EXPECT_FALSE(trace_sampler.isContinueSampled()) << "Should be blocked after rate limit exhausted";
}

// Test CounterSampler with rate 2 exact pattern
TEST_F(SamplingTest, CounterSamplerRate2PatternTest) {
    CounterSampler sampler(2);

    // Pattern: true, false, true, false, ...
    for (int cycle = 0; cycle < 5; ++cycle) {
        EXPECT_TRUE(sampler.isSampled()) << "Cycle " << cycle << " first call should be true";
        EXPECT_FALSE(sampler.isSampled()) << "Cycle " << cycle << " second call should be false";
    }
}

// Test TraceSampler with CounterSampler rate 2 + new limiter
TEST_F(SamplingTest, TraceSamplerPartialSamplingWithLimiterTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(2); // 50% sampling
    const int new_tps = 2;
    TraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), new_tps, 0);

    // Pattern: sampler alternates true/false, the fresh bucket has one token
    // Call 1: sampler=true, limiter allows -> true
    EXPECT_TRUE(trace_sampler.isNewSampled());
    // Call 2: sampler=false -> false
    EXPECT_FALSE(trace_sampler.isNewSampled());
    // Call 3: sampler=true, limiter blocks (refill is 1/new_tps of a second) -> false
    EXPECT_FALSE(trace_sampler.isNewSampled());
    // Call 4: sampler=false -> false
    EXPECT_FALSE(trace_sampler.isNewSampled());
    // Call 5: sampler=true, limiter blocks -> false
    EXPECT_FALSE(trace_sampler.isNewSampled());
    // Call 6: sampler=false -> false
    EXPECT_FALSE(trace_sampler.isNewSampled());
}

// Test refill behavior after time passes
TEST_F(SamplingTest, TraceSamplerRefillTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1); // 100% sampling
    const int new_tps = 2;
    TraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), new_tps, 0);
    
    // Exhaust the rate limiter: a fresh bucket holds a single token
    EXPECT_TRUE(trace_sampler.isNewSampled()) << "Initial call should be allowed";
    EXPECT_FALSE(trace_sampler.isNewSampled()) << "Additional call should be blocked";

    // Wait for refill (1 second, i.e. new_tps tokens)
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Should be able to make calls again
    for (int i = 0; i < new_tps; ++i) {
        EXPECT_TRUE(trace_sampler.isNewSampled()) << "After refill call " << i << " should be allowed";
    }
    EXPECT_FALSE(trace_sampler.isNewSampled()) << "Additional call after refill should be blocked";
}

} // namespace pinpoint
