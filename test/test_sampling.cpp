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
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <vector>
#include <future>

namespace pinpoint {

// Mock AgentService for testing
class MockAgentService : public AgentService {
public:
    MockAgentService() : start_time_(1234567890), cached_start_time_str_(std::to_string(start_time_)) {}
    
    bool isExiting() const override { return false; }
    std::string getAppName() const override { return "TestApp"; }
    int32_t getAppType() const override { return 1300; }
    std::string getAgentId() const override { return "test-agent"; }
    std::string getAgentName() const override { return "Test Agent"; }
    std::shared_ptr<const Config> getConfig() const override { return config_; }
    int64_t getStartTime() const override { return start_time_; }
    void reloadConfig(std::shared_ptr<const Config> cfg) override {
        if (cfg) {
            *config_ = *cfg;
        }
    }
    
    TraceId generateTraceId() override { return TraceId{}; }
    void recordSpan(std::unique_ptr<SpanChunk> span) const override {}
    void recordUrlStat(std::unique_ptr<UrlStatEntry> stat) const override {}
    void recordException(SpanData* span_data) const override {}
    void recordStats(StatsType stats) const override {}
    
    int32_t cacheApi(std::string_view api_str, int32_t api_type) const override { return 0; }
    void removeCacheApi(const ApiMeta& api_meta) const override {}
    int32_t cacheError(std::string_view error_name) const override { return 0; }
    void removeCacheError(const StringMeta& error_meta) const override {}
    int32_t cacheSql(std::string_view sql_query) const override { return 0; }
    void removeCacheSql(const StringMeta& sql_meta) const override {}
    std::vector<unsigned char> cacheSqlUid(std::string_view sql) const override { return {}; }
    void removeCacheSqlUid(const SqlUidMeta& sql_uid_meta) const override {}
    
    bool isStatusFail(int status) const override { return status >= 400; }
    void recordServerHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const override {}
    void recordClientHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const override {}
    
    AgentStats& getAgentStats() override {
        if (!agent_stats_) {
            agent_stats_ = std::make_unique<AgentStats>(this);
        }
        return *agent_stats_;
    }

    UrlStats& getUrlStats() override {
        if (!url_stats_) {
            url_stats_ = std::make_unique<UrlStats>(this);
        }
        return *url_stats_;
    }
    
private:
    int64_t start_time_;
    std::string cached_start_time_str_;
    std::shared_ptr<Config> config_ = std::make_shared<Config>();
    mutable std::unique_ptr<AgentStats> agent_stats_;
    mutable std::unique_ptr<UrlStats> url_stats_;
};

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

// Test CounterSampler with rate N - should return true every Nth call
TEST_F(SamplingTest, CounterSamplerNRateTest) {
    const int rate = 3;
    CounterSampler sampler(rate);
    
    // Test pattern: false, false, true, false, false, true, ...
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (int i = 0; i < rate; ++i) {
            bool expected = (i == rate - 1);
            EXPECT_EQ(sampler.isSampled(), expected) 
                << "Cycle " << cycle << ", call " << i << " should return " << expected;
        }
    }
}

// Test CounterSampler thread safety
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
    
    // Collect results
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

// Test PercentSampler with rate 100.0 - should return true approximately 100% of the time
TEST_F(SamplingTest, PercentSamplerFullRateTest) {
    PercentSampler sampler(100.0); // 100.0 means 100%
    
    int true_count = 0;
    int total_calls = 10000;
    
    for (int i = 0; i < total_calls; ++i) {
        if (sampler.isSampled()) {
            true_count++;
        }
    }
    
    double actual_rate = static_cast<double>(true_count) / total_calls;
    // With rate 100.0, we expect close to 100% sampling
    EXPECT_GT(actual_rate, 0.95) << "Rate 100.0 should result in >95% sampling, got " << actual_rate;
}

// Test PercentSampler with various rates
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

// Test PercentSampler thread safety
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
    
    // Collect results
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

// BasicTraceSampler Tests

// Test BasicTraceSampler with null sampler
TEST_F(SamplingTest, BasicTraceSamplerNullSamplerTest) {
    BasicTraceSampler trace_sampler(mock_service_.get(), nullptr);
    
    // Should always return false for new samples when sampler is null
    EXPECT_FALSE(trace_sampler.isNewSampled());
    EXPECT_FALSE(trace_sampler.isNewSampled());
    
    // Should always return true for continue samples
    EXPECT_TRUE(trace_sampler.isContinueSampled());
    EXPECT_TRUE(trace_sampler.isContinueSampled());
}

// Test BasicTraceSampler with CounterSampler
TEST_F(SamplingTest, BasicTraceSamplerWithCounterSamplerTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(2);
    BasicTraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler));
    
    // Test pattern: false, true, false, true, ...
    EXPECT_FALSE(trace_sampler.isNewSampled());
    EXPECT_TRUE(trace_sampler.isNewSampled());
    EXPECT_FALSE(trace_sampler.isNewSampled());
    EXPECT_TRUE(trace_sampler.isNewSampled());
    
    // Continue samples should always be true
    EXPECT_TRUE(trace_sampler.isContinueSampled());
    EXPECT_TRUE(trace_sampler.isContinueSampled());
}

// Test BasicTraceSampler with PercentSampler
TEST_F(SamplingTest, BasicTraceSamplerWithPercentSamplerTest) {
    auto percent_sampler = std::make_unique<PercentSampler>(10.0); // 10.0 input = 10% sampling
    BasicTraceSampler trace_sampler(mock_service_.get(), std::move(percent_sampler));
    
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

// ThroughputLimitTraceSampler Tests

// Test ThroughputLimitTraceSampler with no limiters
TEST_F(SamplingTest, ThroughputLimitTraceSamplerNoLimitersTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1); // 100% sampling
    ThroughputLimitTraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), 0, 0); // No limiters
    
    // Should follow sampler behavior without rate limiting
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(trace_sampler.isNewSampled()) << "New call " << i << " should be sampled";
        EXPECT_TRUE(trace_sampler.isContinueSampled()) << "Continue call " << i << " should be sampled";
    }
}

// Test ThroughputLimitTraceSampler with new_tps limiter
TEST_F(SamplingTest, ThroughputLimitTraceSamplerNewLimiterTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1); // 100% sampling
    const int new_tps = 3;
    ThroughputLimitTraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), new_tps, 0);
    
    // First new_tps calls should be allowed, then blocked
    for (int i = 0; i < new_tps; ++i) {
        EXPECT_TRUE(trace_sampler.isNewSampled()) << "New call " << i << " should be allowed";
    }
    
    // Next calls should be blocked by rate limiter
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(trace_sampler.isNewSampled()) << "New call " << (new_tps + i) << " should be blocked";
    }
    
    // Continue samples should not be limited (no continue limiter)
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(trace_sampler.isContinueSampled()) << "Continue call " << i << " should be sampled";
    }
}

// Test ThroughputLimitTraceSampler with continue_tps limiter
TEST_F(SamplingTest, ThroughputLimitTraceSamplerContinueLimiterTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1); // 100% sampling
    const int continue_tps = 2;
    ThroughputLimitTraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), 0, continue_tps);
    
    // New samples should not be limited (no new limiter)
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(trace_sampler.isNewSampled()) << "New call " << i << " should be sampled";
    }
    
    // First continue_tps calls should be allowed, then blocked
    for (int i = 0; i < continue_tps; ++i) {
        EXPECT_TRUE(trace_sampler.isContinueSampled()) << "Continue call " << i << " should be allowed";
    }
    
    // Next continue calls should be blocked by rate limiter
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(trace_sampler.isContinueSampled()) << "Continue call " << (continue_tps + i) << " should be blocked";
    }
}

// Test ThroughputLimitTraceSampler with both limiters
TEST_F(SamplingTest, ThroughputLimitTraceSamplerBothLimitersTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1); // 100% sampling
    const int new_tps = 2;
    const int continue_tps = 3;
    ThroughputLimitTraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), new_tps, continue_tps);
    
    // Test new samples with limiter
    for (int i = 0; i < new_tps; ++i) {
        EXPECT_TRUE(trace_sampler.isNewSampled()) << "New call " << i << " should be allowed";
    }
    EXPECT_FALSE(trace_sampler.isNewSampled()) << "Additional new call should be blocked";
    
    // Test continue samples with limiter
    for (int i = 0; i < continue_tps; ++i) {
        EXPECT_TRUE(trace_sampler.isContinueSampled()) << "Continue call " << i << " should be allowed";
    }
    EXPECT_FALSE(trace_sampler.isContinueSampled()) << "Additional continue call should be blocked";
}

// Test ThroughputLimitTraceSampler with sampler that blocks
TEST_F(SamplingTest, ThroughputLimitTraceSamplerWithBlockingSamplerTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(0); // 0% sampling (always blocks)
    ThroughputLimitTraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), 10, 10);
    
    // Should be blocked by sampler before reaching rate limiter
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(trace_sampler.isNewSampled()) << "New call " << i << " should be blocked by sampler";
    }
    
    // Continue samples should still be allowed (rate limiter only)
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(trace_sampler.isContinueSampled()) << "Continue call " << i << " should be allowed";
    }
    EXPECT_FALSE(trace_sampler.isContinueSampled()) << "Additional continue call should be blocked by rate limiter";
}

// Edge Case Tests

// Test CounterSampler with negative rate - should behave like rate 0
TEST_F(SamplingTest, CounterSamplerNegativeRateTest) {
    CounterSampler sampler(-1);

    // Negative rate stored as int; modulo with negative is implementation-defined
    // but should not crash
    for (int i = 0; i < 10; ++i) {
        sampler.isSampled(); // Just verify it doesn't crash
    }
}

// Test CounterSampler with very large rate
TEST_F(SamplingTest, CounterSamplerLargeRateTest) {
    const int rate = 1000000;
    CounterSampler sampler(rate);

    // First (rate - 1) calls should return false
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(sampler.isSampled()) << "Call " << i << " should return false for large rate";
    }
}

// Test PercentSampler with negative rate - should never sample
TEST_F(SamplingTest, PercentSamplerNegativeRateTest) {
    PercentSampler sampler(-5.0);

    // Negative rate becomes negative int; should not crash
    for (int i = 0; i < 100; ++i) {
        sampler.isSampled(); // Just verify no crash
    }
}

// Test PercentSampler with rate exceeding 100.0
TEST_F(SamplingTest, PercentSamplerOverMaxRateTest) {
    PercentSampler sampler(200.0); // 200% - double the max

    int true_count = 0;
    const int total_calls = 10000;

    for (int i = 0; i < total_calls; ++i) {
        if (sampler.isSampled()) {
            true_count++;
        }
    }

    // Even with rate > 100%, should not crash and sampling should work
    EXPECT_GT(true_count, 0) << "Should still sample some requests with over-max rate";
}

// Test PercentSampler with very small rate (0.01 = 0.01%)
TEST_F(SamplingTest, PercentSamplerVerySmallRateTest) {
    PercentSampler sampler(0.01); // 0.01% sampling

    int true_count = 0;
    const int total_calls = 1000000;

    for (int i = 0; i < total_calls; ++i) {
        if (sampler.isSampled()) {
            true_count++;
        }
    }

    // 0.01% of 1M = ~100, allow wide tolerance
    double actual_rate = static_cast<double>(true_count) / total_calls;
    EXPECT_NEAR(actual_rate, 0.0001, 0.0005)
        << "Very small rate should produce very few samples, got " << true_count;
}

// Test PercentSampler with rate 50.0 (50% sampling)
TEST_F(SamplingTest, PercentSamplerHalfRateTest) {
    PercentSampler sampler(50.0);

    int true_count = 0;
    const int total_calls = 100000;

    for (int i = 0; i < total_calls; ++i) {
        if (sampler.isSampled()) {
            true_count++;
        }
    }

    double actual_rate = static_cast<double>(true_count) / total_calls;
    EXPECT_NEAR(actual_rate, 0.50, 0.02) << "50% rate should sample about half, got " << actual_rate;
}

// AgentStats Counter Verification Tests

// Test that BasicTraceSampler correctly increments sample_new stats
TEST_F(SamplingTest, BasicTraceSamplerStatsIncrementNewSampledTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1); // always sampled
    BasicTraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler));
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

// Test that BasicTraceSampler correctly increments unsample_new stats
TEST_F(SamplingTest, BasicTraceSamplerStatsIncrementNewUnsampledTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(0); // never sampled
    BasicTraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler));
    auto& stats = mock_service_->getAgentStats();

    for (int i = 0; i < 5; ++i) {
        trace_sampler.isNewSampled();
    }

    AgentStatsSnapshot snapshot;
    stats.collectAgentStat(snapshot);
    EXPECT_EQ(snapshot.num_sample_new_, 0) << "Should have 0 sampled new traces";
    EXPECT_EQ(snapshot.num_unsample_new_, 5) << "Should have 5 unsampled new traces";
}

// Test that BasicTraceSampler correctly increments sample_cont stats
TEST_F(SamplingTest, BasicTraceSamplerStatsIncrementContTest) {
    BasicTraceSampler trace_sampler(mock_service_.get(), nullptr);
    auto& stats = mock_service_->getAgentStats();

    for (int i = 0; i < 7; ++i) {
        trace_sampler.isContinueSampled();
    }

    AgentStatsSnapshot snapshot;
    stats.collectAgentStat(snapshot);
    EXPECT_EQ(snapshot.num_sample_cont_, 7) << "Should have 7 sampled continue traces";
}

// Test ThroughputLimitTraceSampler skip_new stats when rate limited
TEST_F(SamplingTest, ThroughputLimitTraceSamplerStatsSkipNewTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1); // always sampled
    const int new_tps = 2;
    ThroughputLimitTraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), new_tps, 0);
    auto& stats = mock_service_->getAgentStats();

    // First 2 pass, next 3 get rate limited (skipped)
    for (int i = 0; i < 5; ++i) {
        trace_sampler.isNewSampled();
    }

    AgentStatsSnapshot snapshot;
    stats.collectAgentStat(snapshot);
    EXPECT_EQ(snapshot.num_sample_new_, new_tps) << "Should have " << new_tps << " sampled new traces";
    EXPECT_EQ(snapshot.num_skip_new_, 3) << "Should have 3 skipped new traces";
    EXPECT_EQ(snapshot.num_unsample_new_, 0) << "Should have 0 unsampled new traces";
}

// Test ThroughputLimitTraceSampler skip_cont stats when rate limited
TEST_F(SamplingTest, ThroughputLimitTraceSamplerStatsSkipContTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1);
    const int continue_tps = 3;
    ThroughputLimitTraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), 0, continue_tps);
    auto& stats = mock_service_->getAgentStats();

    // First 3 pass, next 4 get rate limited
    for (int i = 0; i < 7; ++i) {
        trace_sampler.isContinueSampled();
    }

    AgentStatsSnapshot snapshot;
    stats.collectAgentStat(snapshot);
    EXPECT_EQ(snapshot.num_sample_cont_, continue_tps) << "Should have " << continue_tps << " sampled continue traces";
    EXPECT_EQ(snapshot.num_skip_cont_, 4) << "Should have 4 skipped continue traces";
}

// Test ThroughputLimitTraceSampler unsample_new stats when sampler rejects
TEST_F(SamplingTest, ThroughputLimitTraceSamplerStatsUnsampleNewTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(0); // never sampled
    ThroughputLimitTraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), 10, 10);
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

// Test ThroughputLimitTraceSampler with null sampler
TEST_F(SamplingTest, ThroughputLimitTraceSamplerNullSamplerTest) {
    ThroughputLimitTraceSampler trace_sampler(mock_service_.get(), nullptr, 5, 5);

    // Null sampler should always return false for new samples
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(trace_sampler.isNewSampled()) << "Null sampler should reject new sample " << i;
    }

    // Continue samples should still be allowed (up to rate limit)
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(trace_sampler.isContinueSampled()) << "Continue call " << i << " should be allowed";
    }
    EXPECT_FALSE(trace_sampler.isContinueSampled()) << "Should be blocked after rate limit exhausted";
}

// Test CounterSampler with rate 2 exact pattern
TEST_F(SamplingTest, CounterSamplerRate2PatternTest) {
    CounterSampler sampler(2);

    // Pattern: false, true, false, true, ...
    for (int cycle = 0; cycle < 5; ++cycle) {
        EXPECT_FALSE(sampler.isSampled()) << "Cycle " << cycle << " first call should be false";
        EXPECT_TRUE(sampler.isSampled()) << "Cycle " << cycle << " second call should be true";
    }
}

// Test ThroughputLimitTraceSampler with CounterSampler rate 2 + new limiter
TEST_F(SamplingTest, ThroughputLimitTraceSamplerPartialSamplingWithLimiterTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(2); // 50% sampling
    const int new_tps = 2;
    ThroughputLimitTraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), new_tps, 0);

    // Pattern: sampler alternates false/true, rate limiter allows first 2 true results
    // Call 1: sampler=false -> false
    EXPECT_FALSE(trace_sampler.isNewSampled());
    // Call 2: sampler=true, limiter allows -> true
    EXPECT_TRUE(trace_sampler.isNewSampled());
    // Call 3: sampler=false -> false
    EXPECT_FALSE(trace_sampler.isNewSampled());
    // Call 4: sampler=true, limiter allows -> true
    EXPECT_TRUE(trace_sampler.isNewSampled());
    // Call 5: sampler=false -> false
    EXPECT_FALSE(trace_sampler.isNewSampled());
    // Call 6: sampler=true, limiter blocks -> false
    EXPECT_FALSE(trace_sampler.isNewSampled());
}

// Test refill behavior after time passes
TEST_F(SamplingTest, ThroughputLimitTraceSamplerRefillTest) {
    auto counter_sampler = std::make_unique<CounterSampler>(1); // 100% sampling
    const int new_tps = 2;
    ThroughputLimitTraceSampler trace_sampler(mock_service_.get(), std::move(counter_sampler), new_tps, 0);
    
    // Exhaust the rate limiter
    for (int i = 0; i < new_tps; ++i) {
        EXPECT_TRUE(trace_sampler.isNewSampled()) << "Initial call " << i << " should be allowed";
    }
    EXPECT_FALSE(trace_sampler.isNewSampled()) << "Additional call should be blocked";
    
    // Wait for refill (1 second)
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // Should be able to make calls again
    for (int i = 0; i < new_tps; ++i) {
        EXPECT_TRUE(trace_sampler.isNewSampled()) << "After refill call " << i << " should be allowed";
    }
    EXPECT_FALSE(trace_sampler.isNewSampled()) << "Additional call after refill should be blocked";
}

} // namespace pinpoint
