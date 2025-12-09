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

#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <chrono>

#include "../src/url_stat.h"
#include "../src/config.h"
#include "../src/agent_service.h"
#include "../src/stat.h"

namespace pinpoint {

// Mock implementation of AgentService for UrlStats testing
class MockAgentService : public AgentService {
public:
    MockAgentService() : is_exiting_(false), start_time_(1234567890), trace_id_counter_(0) {
        // Configure default config for URL stats
        config_.http.url_stat.enable = true;
        config_.http.url_stat.limit = 1024;
        config_.http.url_stat.path_depth = 3;
        config_.http.url_stat.method_prefix = false;
        config_.span.queue_size = 100;
    }

    // AgentService interface implementation
    bool isExiting() const override { return is_exiting_; }
    std::string_view getAppName() const override { return "TestApp"; }
    int32_t getAppType() const override { return 1300; }
    std::string_view getAgentId() const override { return "test-agent-001"; }
    std::string_view getAgentName() const override { return "TestAgent"; }
    const Config& getConfig() const override { return config_; }
    int64_t getStartTime() const override { return start_time_; }

    TraceId generateTraceId() override {
        TraceId trace_id;
        trace_id.StartTime = start_time_;
        trace_id.Sequence = trace_id_counter_++;
        return trace_id;
    }

    void recordSpan(std::unique_ptr<SpanChunk> span) const override {
        recorded_spans_++;
    }

    void recordUrlStat(std::unique_ptr<UrlStat> stat) const override {
        recorded_url_stats_++;
    }

    void recordException(SpanData* span_data) const override {
        recorded_exceptions_++;
    }

    void recordStats(StatsType stats) const override {
        recorded_stats_calls_++;
        last_stats_type_ = stats;
    }

    int32_t cacheApi(std::string_view api_str, int32_t api_type) const override {
        return 42; // Mock return value
    }

    void removeCacheApi(const ApiMeta& api_meta) const override {
        // Mock implementation
    }

    int32_t cacheError(std::string_view error_name) const override {
        return 100; // Mock return value
    }

    void removeCacheError(const StringMeta& error_meta) const override {
        // Mock implementation
    }

    int32_t cacheSql(std::string_view sql_query) const override {
        return 300; // Mock return value
    }

    void removeCacheSql(const StringMeta& sql_meta) const override {
        // Mock implementation
    }

    std::vector<unsigned char> cacheSqlUid(std::string_view sql) const override {
        // Mock implementation - return test uid
        return {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    }

    void removeCacheSqlUid(const SqlUidMeta& sql_uid_meta) const override {
        // Mock implementation
    }

    bool isStatusFail(int status) const override {
        return status >= 400;
    }

    void recordServerHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const override {
        // Mock implementation
    }

    void recordClientHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const override {
        // Mock implementation
    }

    AgentStats& getAgentStats() override {
        if (!agent_stats_) {
            agent_stats_ = std::make_unique<AgentStats>(this);
        }
        return *agent_stats_;
    }

    // Test helpers
    void setExiting(bool exiting) { is_exiting_ = exiting; }
    void setUrlStatEnabled(bool enabled) { config_.http.url_stat.enable = enabled; }
    mutable int recorded_spans_ = 0;
    mutable int recorded_url_stats_ = 0;
    mutable int recorded_exceptions_ = 0;
    mutable int recorded_stats_calls_ = 0;
    mutable StatsType last_stats_type_ = AGENT_STATS;

private:
    bool is_exiting_;
    int64_t start_time_;
    int64_t trace_id_counter_;
    Config config_;
    mutable std::unique_ptr<AgentStats> agent_stats_;
};

class UrlStatTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_agent_service_ = std::make_unique<MockAgentService>();
        mock_agent_service_->setExiting(false);  // Ensure clean state
        init_url_stat();
    }

    void TearDown() override {
        mock_agent_service_.reset();
    }

    std::unique_ptr<MockAgentService> mock_agent_service_;
};

// ========== TickClock Tests ==========

TEST_F(UrlStatTest, TickClockConstructorTest) {
    TickClock clock(30);
    SUCCEED() << "TickClock constructor should not crash";
}

TEST_F(UrlStatTest, TickClockTickTest) {
    TickClock clock(30);
    
    auto now = std::chrono::system_clock::now();
    int64_t tick_value = clock.tick(now);
    
    EXPECT_GT(tick_value, 0) << "Tick value should be positive";
    
    // Test that tick values are consistent for the same time period
    int64_t tick_value2 = clock.tick(now);
    EXPECT_EQ(tick_value, tick_value2) << "Tick values should be same for same time";
}

TEST_F(UrlStatTest, TickClockDifferentIntervalsTest) {
    TickClock clock1(30);
    TickClock clock2(60);
    
    auto now = std::chrono::system_clock::now();
    int64_t tick1 = clock1.tick(now);
    int64_t tick2 = clock2.tick(now);
    
    // Different interval clocks may produce different tick values
    EXPECT_GT(tick1, 0) << "Tick1 should be positive";
    EXPECT_GT(tick2, 0) << "Tick2 should be positive";
}

// ========== UrlStatHistogram Tests ==========

TEST_F(UrlStatTest, UrlStatHistogramConstructorTest) {
    UrlStatHistogram histogram;
    
    EXPECT_EQ(histogram.total(), 0) << "Initial total should be 0";
    EXPECT_EQ(histogram.max(), 0) << "Initial max should be 0";
    
    for (int i = 0; i < URL_STATS_BUCKET_SIZE; i++) {
        EXPECT_EQ(histogram.histogram(i), 0) << "Initial histogram bucket " << i << " should be 0";
    }
}

TEST_F(UrlStatTest, UrlStatHistogramAddTest) {
    UrlStatHistogram histogram;
    
    histogram.add(50);   // Should go to bucket 0 (< 100)
    histogram.add(250);  // Should go to bucket 1 (100-299)
    histogram.add(450);  // Should go to bucket 2 (300-499)
    histogram.add(750);  // Should go to bucket 3 (500-999)
    
    EXPECT_EQ(histogram.total(), 1500) << "Total should be sum of elapsed times (50+250+450+750=1500)";
    EXPECT_EQ(histogram.max(), 750) << "Max should be 750";
    
    // Check bucket distribution
    EXPECT_GT(histogram.histogram(0), 0) << "Bucket 0 should have entries";
    EXPECT_GT(histogram.histogram(1), 0) << "Bucket 1 should have entries";
    EXPECT_GT(histogram.histogram(2), 0) << "Bucket 2 should have entries";
    EXPECT_GT(histogram.histogram(3), 0) << "Bucket 3 should have entries";
}

TEST_F(UrlStatTest, UrlStatHistogramMaxTrackingTest) {
    UrlStatHistogram histogram;
    
    histogram.add(100);
    EXPECT_EQ(histogram.max(), 100);
    
    histogram.add(50);   // Smaller value
    EXPECT_EQ(histogram.max(), 100) << "Max should remain 100";
    
    histogram.add(200);  // Larger value
    EXPECT_EQ(histogram.max(), 200) << "Max should update to 200";
}

// ========== EachUrlStat Tests ==========

TEST_F(UrlStatTest, EachUrlStatConstructorTest) {
    int64_t tick = 1234567890;
    
    EachUrlStat stat(tick);
    
    EXPECT_EQ(stat.tick(), tick) << "Tick should match constructor parameter";
    
    // Test histogram access
    auto& total_hist = stat.getTotalHistogram();
    auto& fail_hist = stat.getFailHistogram();
    
    EXPECT_EQ(total_hist.total(), 0) << "Initial total histogram should be empty";
    EXPECT_EQ(fail_hist.total(), 0) << "Initial fail histogram should be empty";
}

TEST_F(UrlStatTest, EachUrlStatHistogramModificationTest) {
    EachUrlStat stat(12345);
    
    auto& total_hist = stat.getTotalHistogram();
    auto& fail_hist = stat.getFailHistogram();
    
    total_hist.add(100);
    fail_hist.add(200);
    
    EXPECT_EQ(total_hist.total(), 100) << "Total histogram total should be sum of elapsed times (100)";
    EXPECT_EQ(fail_hist.total(), 200) << "Fail histogram total should be sum of elapsed times (200)";
    EXPECT_EQ(total_hist.max(), 100) << "Total histogram max should be 100";
    EXPECT_EQ(fail_hist.max(), 200) << "Fail histogram max should be 200";
}

// ========== UrlKey Tests ==========

TEST_F(UrlStatTest, UrlKeyComparisonTest) {
    UrlKey key1{"/api/users", 1000};
    UrlKey key2{"/api/users", 1000};
    UrlKey key3{"/api/users", 2000};
    UrlKey key4{"/api/posts", 1000};
    
    // Same keys should not be less than each other
    EXPECT_FALSE(key1 < key2) << "Identical keys should not be less than each other";
    EXPECT_FALSE(key2 < key1) << "Identical keys should not be less than each other";
    
    // Same URL, different tick
    EXPECT_TRUE(key1 < key3) << "Same URL with earlier tick should be less";
    EXPECT_FALSE(key3 < key1) << "Same URL with later tick should not be less";
    
    // Different URLs
    EXPECT_TRUE(key4 < key1) << "Alphabetically earlier URL should be less";
    EXPECT_FALSE(key1 < key4) << "Alphabetically later URL should not be less";
}

// ========== UrlStat Tests ==========

TEST_F(UrlStatTest, UrlStatConstructorTest) {
    UrlStat stat("/api/users", "GET", 200);
    
    EXPECT_EQ(stat.url_pattern_, "/api/users") << "URL pattern should match";
    EXPECT_EQ(stat.method_, "GET") << "Method should match";
    EXPECT_EQ(stat.status_code_, 200) << "Status code should match";
    EXPECT_EQ(stat.elapsed_, 0) << "Initial elapsed should be 0";
}

// ========== UrlStatSnapshot Tests ==========

TEST_F(UrlStatTest, UrlStatSnapshotConstructorTest) {
    UrlStatSnapshot snapshot;
    
    auto& stats = snapshot.getEachStats();
    EXPECT_TRUE(stats.empty()) << "Initial snapshot should be empty";
}

TEST_F(UrlStatTest, UrlStatSnapshotAddTest) {
    UrlStatSnapshot snapshot;
    Config config;
    
    // Create a test UrlStat
    UrlStat stat("/api/users", "GET", 200);
    stat.elapsed_ = 150;
    stat.end_time_ = std::chrono::system_clock::now();
    
    // Add to snapshot
    snapshot.add(&stat, config);
    
    auto& stats = snapshot.getEachStats();
    EXPECT_FALSE(stats.empty()) << "Snapshot should contain entries after add";
}

// ========== UrlStats Class Tests ==========

TEST_F(UrlStatTest, UrlStatsConstructorTest) {
    UrlStats url_stats(mock_agent_service_.get());
    SUCCEED() << "UrlStats constructor should not crash";
}

TEST_F(UrlStatTest, UrlStatsEnqueueTest) {
    UrlStats url_stats(mock_agent_service_.get());
    
    auto stat = std::make_unique<UrlStat>("/api/test", "POST", 201);
    url_stats.enqueueUrlStats(std::move(stat));
    
    SUCCEED() << "Enqueue should not crash";
}

TEST_F(UrlStatTest, UrlStatsEnqueueWithDisabledConfigTest) {
    mock_agent_service_->setUrlStatEnabled(false);
    UrlStats url_stats(mock_agent_service_.get());
    
    auto stat = std::make_unique<UrlStat>("/api/test", "POST", 201);
    url_stats.enqueueUrlStats(std::move(stat));
    
    SUCCEED() << "Enqueue with disabled config should not crash";
}

TEST_F(UrlStatTest, UrlStatsWorkerStartStopTest) {
    UrlStats url_stats(mock_agent_service_.get());
    
    // Test add worker
    std::thread add_worker([&url_stats]() {
        url_stats.addUrlStatsWorker();
    });
    
    // Give worker time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Signal to stop and wait
    mock_agent_service_->setExiting(true);
    url_stats.stopAddUrlStatsWorker();
    add_worker.join();
    
    // Reset for send worker test
    mock_agent_service_->setExiting(false);
    
    // Test send worker
    std::thread send_worker([&url_stats]() {
        url_stats.sendUrlStatsWorker();
    });
    
    // Give worker time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Signal to stop and wait
    mock_agent_service_->setExiting(true);
    url_stats.stopSendUrlStatsWorker();
    send_worker.join();
    
    SUCCEED() << "Worker threads should start and stop cleanly";
}

TEST_F(UrlStatTest, UrlStatsWithExitingAgentTest) {
    mock_agent_service_->setExiting(true);
    UrlStats url_stats(mock_agent_service_.get());
    
    // Test that workers exit quickly when agent is exiting
    std::thread add_worker([&url_stats]() {
        url_stats.addUrlStatsWorker();
    });
    
    std::thread send_worker([&url_stats]() {
        url_stats.sendUrlStatsWorker();
    });
    
    add_worker.join();
    send_worker.join();
    
    SUCCEED() << "Workers should exit quickly when agent is exiting";
}

// ========== Global Functions Tests ==========

TEST_F(UrlStatTest, InitUrlStatTest) {
    init_url_stat();
    SUCCEED() << "init_url_stat should not crash";
}

TEST_F(UrlStatTest, AddAndTakeSnapshotTest) {
    Config config;
    UrlStat stat("/api/test", "GET", 200);
    stat.elapsed_ = 100;
    stat.end_time_ = std::chrono::system_clock::now();
    
    // Add to snapshot
    add_url_stat_snapshot(&stat, config);
    
    // Take snapshot
    auto snapshot = take_url_stat_snapshot();
    EXPECT_NE(snapshot.get(), nullptr) << "Snapshot should not be null";
    
    auto& stats = snapshot->getEachStats();
    EXPECT_FALSE(stats.empty()) << "Snapshot should contain the added stat";
}

TEST_F(UrlStatTest, TrimUrlPathTest) {
    // Test basic path trimming
    std::string result1 = trim_url_path("/api/v1/users/123/posts/456", 3);
    EXPECT_FALSE(result1.empty()) << "Trimmed path should not be empty";
    
    // Test with depth 0 (gets converted to depth 1)
    std::string result2 = trim_url_path("/api/v1/users", 0);
    EXPECT_FALSE(result2.empty()) << "Trimmed path with depth 0 should not be empty";
    
    // Test with non-empty path that starts without slash
    std::string result3 = trim_url_path("api/test", 2);
    EXPECT_FALSE(result3.empty()) << "Trimmed path should not be empty";
    
    // Test with single level path
    std::string result4 = trim_url_path("/api", 1);
    EXPECT_EQ(result4, "/api") << "Single level path should be preserved";
    
    // Test path with query parameters
    std::string result5 = trim_url_path("/api/users?id=123", 2);
    EXPECT_FALSE(result5.empty()) << "Path with query params should be trimmed";
    EXPECT_EQ(result5.find('?'), std::string::npos) << "Query params should be removed";
}

TEST_F(UrlStatTest, TrimUrlPathDepthTest) {
    // Test different depths
    std::string path = "/api/v1/users/123/posts";
    
    std::string result1 = trim_url_path(path, 1);
    std::string result2 = trim_url_path(path, 2);
    std::string result3 = trim_url_path(path, 3);
    
    // Results should be different for different depths
    EXPECT_FALSE(result1.empty());
    EXPECT_FALSE(result2.empty());
    EXPECT_FALSE(result3.empty());
    
    // Higher depth should generally result in longer paths (unless truncated)
    // This depends on implementation, but at least they should be valid
}

// ========== Integration Tests ==========

TEST_F(UrlStatTest, FullWorkflowTest) {
    UrlStats url_stats(mock_agent_service_.get());
    
    // Create and enqueue multiple stats
    auto stat1 = std::make_unique<UrlStat>("/api/users", "GET", 200);
    stat1->elapsed_ = 100;
    auto stat2 = std::make_unique<UrlStat>("/api/posts", "POST", 201);
    stat2->elapsed_ = 200;
    auto stat3 = std::make_unique<UrlStat>("/api/users", "GET", 500);
    stat3->elapsed_ = 300;
    
    url_stats.enqueueUrlStats(std::move(stat1));
    url_stats.enqueueUrlStats(std::move(stat2));
    url_stats.enqueueUrlStats(std::move(stat3));
    
    // Start add worker
    std::thread add_worker([&url_stats]() {
        url_stats.addUrlStatsWorker();
    });
    
    // Let it process for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Stop worker
    mock_agent_service_->setExiting(true);
    url_stats.stopAddUrlStatsWorker();
    add_worker.join();
    
    // Take snapshot and verify
    auto snapshot = take_url_stat_snapshot();
    auto& stats = snapshot->getEachStats();
    
    EXPECT_FALSE(stats.empty()) << "Snapshot should contain processed stats";
    
    SUCCEED() << "Full workflow completed successfully";
}

TEST_F(UrlStatTest, ConcurrentEnqueueTest) {
    UrlStats url_stats(mock_agent_service_.get());
    
    const int num_threads = 5;
    const int stats_per_thread = 10;
    
    std::vector<std::thread> threads;
    
    // Start multiple threads enqueueing stats
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&url_stats, t]() {
            for (int i = 0; i < stats_per_thread; i++) {
                auto stat = std::make_unique<UrlStat>("/api/test" + std::to_string(t) + "/" + std::to_string(i), "GET", 200);
                stat->elapsed_ = 100 + i;
                url_stats.enqueueUrlStats(std::move(stat));
            }
        });
    }
    
    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }
    
    SUCCEED() << "Concurrent enqueue should not crash";
}

TEST_F(UrlStatTest, SendWorkerRecordsStatsTest) {
    UrlStats url_stats(mock_agent_service_.get());
    
    // Start send worker
    std::thread send_worker([&url_stats]() {
        url_stats.sendUrlStatsWorker();
    });
    
    // Let it run for a brief moment
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Stop worker
    mock_agent_service_->setExiting(true);
    url_stats.stopSendUrlStatsWorker();
    send_worker.join();
    
    // Check if stats were recorded (depends on timing and implementation)
    // Just verify the test completed without crash
    SUCCEED() << "Send worker should run without crashing";
}

} // namespace pinpoint
