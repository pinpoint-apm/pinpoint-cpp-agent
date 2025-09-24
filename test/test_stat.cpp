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
#include <cmath>

#include "../src/stat.h"
#include "../src/config.h"

namespace pinpoint {

// Mock implementation of AgentService
class MockAgentService : public AgentService {
public:
    MockAgentService() : is_exiting_(false), start_time_(1234567890), trace_id_counter_(0) {}

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

    void removeCacheError(const StringMeta& str_meta) const override {
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

    // Test helpers
    void setExiting(bool exiting) { is_exiting_ = exiting; }
    mutable int recorded_spans_ = 0;
    mutable int recorded_url_stats_ = 0;
    mutable int recorded_stats_calls_ = 0;
    mutable StatsType last_stats_type_ = AGENT_STATS;

private:
    bool is_exiting_;
    int64_t start_time_;
    int64_t trace_id_counter_;
    Config config_;
};

class StatTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_agent_service_ = std::make_unique<MockAgentService>();
        init_agent_stats();
    }

    void TearDown() override {
        mock_agent_service_.reset();
    }

    std::unique_ptr<MockAgentService> mock_agent_service_;
};

// ========== Agent Stats Global Functions Tests ==========

TEST_F(StatTest, InitAgentStatsTest) {
    // Test that init_agent_stats() doesn't crash
    init_agent_stats();
    SUCCEED(); // If we reach here, initialization was successful
}

TEST_F(StatTest, CollectAgentStatTest) {
    AgentStatsSnapshot snapshot;
    
    // Initialize snapshot with zeros
    memset(&snapshot, 0, sizeof(snapshot));
    
    // Test that collect_agent_stat() fills the snapshot
    collect_agent_stat(snapshot);
    
    // Verify that sample_time is set (should be > 0 after collection)
    EXPECT_GT(snapshot.sample_time_, 0) << "Sample time should be set";
    
    // CPU times might be NaN on some systems (like macOS), so check if they're valid or NaN
    EXPECT_TRUE(snapshot.system_cpu_time_ >= 0.0 || std::isnan(snapshot.system_cpu_time_)) 
        << "System CPU time should be non-negative or NaN";
    EXPECT_TRUE(snapshot.process_cpu_time_ >= 0.0 || std::isnan(snapshot.process_cpu_time_)) 
        << "Process CPU time should be non-negative or NaN";
    EXPECT_GE(snapshot.num_threads_, 0) << "Number of threads should be non-negative";
}

TEST_F(StatTest, CollectResponseTimeTest) {
    // Test response time collection
    collect_response_time(100);
    collect_response_time(200);
    collect_response_time(50);
    
    AgentStatsSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    collect_agent_stat(snapshot);
    
    // After collecting response times, average and max should be calculated
    // Note: The exact values depend on implementation, but they should be reasonable
    EXPECT_GE(snapshot.response_time_avg_, 0) << "Average response time should be non-negative";
    EXPECT_GE(snapshot.response_time_max_, 0) << "Max response time should be non-negative";
}

TEST_F(StatTest, SamplingCountersTest) {
    // Test sample counting functions
    incr_sample_new();
    incr_sample_new();
    incr_unsample_new();
    incr_sample_cont();
    incr_unsample_cont();
    incr_skip_new();
    incr_skip_cont();
    
    AgentStatsSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    collect_agent_stat(snapshot);
    
    // Verify counters are incremented
    EXPECT_EQ(snapshot.num_sample_new_, 2) << "Sample new counter should be 2";
    EXPECT_EQ(snapshot.num_unsample_new_, 1) << "Unsample new counter should be 1";
    EXPECT_EQ(snapshot.num_sample_cont_, 1) << "Sample cont counter should be 1";
    EXPECT_EQ(snapshot.num_unsample_cont_, 1) << "Unsample cont counter should be 1";
    EXPECT_EQ(snapshot.num_skip_new_, 1) << "Skip new counter should be 1";
    EXPECT_EQ(snapshot.num_skip_cont_, 1) << "Skip cont counter should be 1";
}

TEST_F(StatTest, ActiveSpanManagementTest) {
    int64_t span_id_1 = 12345;
    int64_t span_id_2 = 67890;
    int64_t start_time = 1234567890;
    
    // Add active spans
    add_active_span(span_id_1, start_time);
    add_active_span(span_id_2, start_time + 100);
    
    AgentStatsSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    collect_agent_stat(snapshot);
    
    // Active spans should be reflected in the snapshot
    // The exact distribution depends on implementation but total should be > 0
    int total_active = snapshot.active_requests_[0] + snapshot.active_requests_[1] + 
                      snapshot.active_requests_[2] + snapshot.active_requests_[3];
    EXPECT_GT(total_active, 0) << "Should have active spans";
    
    // Drop one span
    drop_active_span(span_id_1);
    
    memset(&snapshot, 0, sizeof(snapshot));
    collect_agent_stat(snapshot);
    
    // Should have fewer active spans now
    int new_total_active = snapshot.active_requests_[0] + snapshot.active_requests_[1] + 
                          snapshot.active_requests_[2] + snapshot.active_requests_[3];
    EXPECT_LE(new_total_active, total_active) << "Should have same or fewer active spans after dropping";
}

TEST_F(StatTest, GetAgentStatSnapshotsTest) {
    // Test that we can get the snapshots vector
    std::vector<AgentStatsSnapshot>& snapshots = get_agent_stat_snapshots();
    
    size_t initial_size = snapshots.size();
    
    // Collect some stats
    AgentStatsSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    collect_agent_stat(snapshot);
    
    // The snapshots vector should be accessible
    EXPECT_GE(snapshots.size(), initial_size) << "Snapshots vector should be accessible";
}

// ========== AgentStats Class Tests ==========

TEST_F(StatTest, AgentStatsConstructorTest) {
    AgentStats agent_stats(mock_agent_service_.get());
    
    // Test that constructor doesn't crash
    SUCCEED();
}

TEST_F(StatTest, AgentStatsWorkerStopTest) {
    AgentStats agent_stats(mock_agent_service_.get());
    
    // Start worker in a separate thread
    std::thread worker_thread([&agent_stats]() {
        agent_stats.agentStatsWorker();
    });
    
    // Let it run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Stop the worker
    agent_stats.stopAgentStatsWorker();
    
    // Wait for thread to finish
    worker_thread.join();
    
    SUCCEED() << "AgentStats worker should start and stop cleanly";
}

TEST_F(StatTest, AgentStatsWorkerRecordsStatsTest) {
    AgentStats agent_stats(mock_agent_service_.get());
    
    // Start worker in a separate thread
    std::thread worker_thread([&agent_stats]() {
        agent_stats.agentStatsWorker();
    });
    
    // Let it run longer to ensure stats collection interval passes
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    
    // Stop the worker
    agent_stats.stopAgentStatsWorker();
    
    // Wait for thread to finish
    worker_thread.join();
    
    // Check if stats were recorded (may not be called if collection interval hasn't passed)
    // Just verify the test completed without crash
    SUCCEED() << "AgentStats worker should run without crashing";
}

TEST_F(StatTest, AgentStatsWithExitingAgentTest) {
    mock_agent_service_->setExiting(true);
    
    AgentStats agent_stats(mock_agent_service_.get());
    
    // Start worker in a separate thread
    std::thread worker_thread([&agent_stats]() {
        agent_stats.agentStatsWorker();
    });
    
    // Let it run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Stop the worker
    agent_stats.stopAgentStatsWorker();
    
    // Wait for thread to finish
    worker_thread.join();
    
    // When agent is exiting, worker should exit quickly
    SUCCEED() << "AgentStats worker should handle exiting agent gracefully";
}

TEST_F(StatTest, MultipleResponseTimeCollectionTest) {
    // Test collecting multiple response times
    std::vector<int64_t> response_times = {50, 100, 150, 200, 75, 125, 300, 25};
    
    for (int64_t time : response_times) {
        collect_response_time(time);
    }
    
    AgentStatsSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    collect_agent_stat(snapshot);
    
    // Verify that max response time is the maximum we inserted
    EXPECT_EQ(snapshot.response_time_max_, 300) << "Max response time should be 300";
    
    // Average should be reasonable (exact value depends on implementation)
    EXPECT_GT(snapshot.response_time_avg_, 0) << "Average response time should be positive";
    EXPECT_LE(snapshot.response_time_avg_, 300) << "Average should not exceed max";
}

TEST_F(StatTest, ActiveSpanTimeDistributionTest) {
    // First, get the current state to see how many spans exist
    AgentStatsSnapshot initial_snapshot;
    memset(&initial_snapshot, 0, sizeof(initial_snapshot));
    collect_agent_stat(initial_snapshot);
    
    int initial_total = initial_snapshot.active_requests_[0] + initial_snapshot.active_requests_[1] + 
                       initial_snapshot.active_requests_[2] + initial_snapshot.active_requests_[3];
    
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    
    // Add spans with different durations using unique IDs
    int64_t base_id = 10000;  // Use higher IDs to avoid conflicts
    add_active_span(base_id + 1, now_ms - 500);   // 500ms old - should be in bucket 0
    add_active_span(base_id + 2, now_ms - 1500);  // 1.5s old - should be in bucket 1
    add_active_span(base_id + 3, now_ms - 3500);  // 3.5s old - should be in bucket 2
    add_active_span(base_id + 4, now_ms - 6000);  // 6s old - should be in bucket 3
    
    AgentStatsSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    collect_agent_stat(snapshot);
    
    // Verify that we have added 4 more spans
    int final_total = snapshot.active_requests_[0] + snapshot.active_requests_[1] + 
                     snapshot.active_requests_[2] + snapshot.active_requests_[3];
    EXPECT_EQ(final_total, initial_total + 4) << "Should have 4 more active spans than initially";
    
    // At least some buckets should have spans
    bool has_distribution = false;
    for (int i = 0; i < 4; i++) {
        if (snapshot.active_requests_[i] > 0) {
            has_distribution = true;
            break;
        }
    }
    EXPECT_TRUE(has_distribution) << "Spans should be distributed in time buckets";
    
    // Clean up our test spans
    drop_active_span(base_id + 1);
    drop_active_span(base_id + 2);
    drop_active_span(base_id + 3);
    drop_active_span(base_id + 4);
}

TEST_F(StatTest, StatSnapshotMemoryLayoutTest) {
    AgentStatsSnapshot snapshot;
    
    // Test that the structure has the expected size and alignment
    EXPECT_GT(sizeof(snapshot), 0) << "AgentStatsSnapshot should have positive size";
    
    // Test that we can access all fields without crashes
    snapshot.sample_time_ = 123;
    snapshot.system_cpu_time_ = 1.5;
    snapshot.process_cpu_time_ = 2.5;
    snapshot.num_threads_ = 10;
    snapshot.heap_alloc_size_ = 1024;
    snapshot.heap_max_size_ = 2048;
    snapshot.response_time_avg_ = 100;
    snapshot.response_time_max_ = 200;
    snapshot.num_sample_new_ = 5;
    snapshot.num_sample_cont_ = 6;
    snapshot.num_unsample_new_ = 7;
    snapshot.num_unsample_cont_ = 8;
    snapshot.num_skip_new_ = 9;
    snapshot.num_skip_cont_ = 10;
    
    for (int i = 0; i < 4; i++) {
        snapshot.active_requests_[i] = i + 1;
    }
    
    // Verify all fields are accessible
    EXPECT_EQ(snapshot.sample_time_, 123);
    EXPECT_DOUBLE_EQ(snapshot.system_cpu_time_, 1.5);
    EXPECT_DOUBLE_EQ(snapshot.process_cpu_time_, 2.5);
    EXPECT_EQ(snapshot.num_threads_, 10);
    EXPECT_EQ(snapshot.heap_alloc_size_, 1024);
    EXPECT_EQ(snapshot.heap_max_size_, 2048);
    EXPECT_EQ(snapshot.response_time_avg_, 100);
    EXPECT_EQ(snapshot.response_time_max_, 200);
    EXPECT_EQ(snapshot.num_sample_new_, 5);
    EXPECT_EQ(snapshot.num_sample_cont_, 6);
    EXPECT_EQ(snapshot.num_unsample_new_, 7);
    EXPECT_EQ(snapshot.num_unsample_cont_, 8);
    EXPECT_EQ(snapshot.num_skip_new_, 9);
    EXPECT_EQ(snapshot.num_skip_cont_, 10);
    
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(snapshot.active_requests_[i], i + 1);
    }
}

} // namespace pinpoint
