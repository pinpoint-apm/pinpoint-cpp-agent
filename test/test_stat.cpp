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
#include <cstring>

#include "../src/stat.h"
#include "../src/url_stat.h"
#include "../src/config.h"
#include "mock_agent_service.h"

namespace pinpoint {

class StatTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_agent_service_ = std::make_unique<MockAgentService>();
        mock_agent_service_->setExiting(false);  // Ensure clean state
        
        // Create AgentStats instance
        agent_stats_ = std::make_unique<AgentStats>(mock_agent_service_.get());
        // Initialize
        agent_stats_->initAgentStats();
    }

    void TearDown() override {
        agent_stats_.reset();
        mock_agent_service_.reset();
    }

    std::unique_ptr<MockAgentService> mock_agent_service_;
    std::unique_ptr<AgentStats> agent_stats_;
};

// ========== Agent Stats Global Functions Tests ==========

TEST_F(StatTest, InitAgentStatsTest) {
    // Test that initAgentStats() doesn't crash
    agent_stats_->initAgentStats();
    SUCCEED(); // If we reach here, initialization was successful
}

TEST_F(StatTest, CollectAgentStatTest) {
    AgentStatsSnapshot snapshot;
    
    // Test that collectAgentStat() fills the snapshot
    agent_stats_->collectAgentStat(snapshot);
    
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
    agent_stats_->collectResponseTime(100);
    agent_stats_->collectResponseTime(200);
    agent_stats_->collectResponseTime(50);
    
    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);
    
    // After collecting response times, average and max should be calculated
    // Note: The exact values depend on implementation, but they should be reasonable
    EXPECT_GE(snapshot.response_time_avg_, 0) << "Average response time should be non-negative";
    EXPECT_GE(snapshot.response_time_max_, 0) << "Max response time should be non-negative";
}

TEST_F(StatTest, SamplingCountersTest) {
    // Test sample counting functions
    agent_stats_->incrSampleNew();
    agent_stats_->incrSampleNew();
    agent_stats_->incrUnsampleNew();
    agent_stats_->incrSampleCont();
    agent_stats_->incrUnsampleCont();
    agent_stats_->incrSkipNew();
    agent_stats_->incrSkipCont();
    
    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);
    
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
    
    // Add active spans directly via AgentStats
    agent_stats_->addActiveSpan(span_id_1, start_time);
    agent_stats_->addActiveSpan(span_id_2, start_time + 100);
    
    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);
    
    // Active spans should be reflected in the snapshot
    // The exact distribution depends on implementation but total should be > 0
    int total_active = snapshot.active_requests_[0] + snapshot.active_requests_[1] + 
                      snapshot.active_requests_[2] + snapshot.active_requests_[3];
    EXPECT_GT(total_active, 0) << "Should have active spans";
    
    // Drop one span
    agent_stats_->dropActiveSpan(span_id_1);
    
    agent_stats_->collectAgentStat(snapshot);
    
    // Should have fewer active spans now
    int new_total_active = snapshot.active_requests_[0] + snapshot.active_requests_[1] + 
                          snapshot.active_requests_[2] + snapshot.active_requests_[3];
    EXPECT_LE(new_total_active, total_active) << "Should have same or fewer active spans after dropping";
}

TEST_F(StatTest, GetAgentStatSnapshotsTest) {
    // Test that we can get the snapshots vector
    std::vector<AgentStatsSnapshot>& snapshots = agent_stats_->getSnapshots();
    
    size_t initial_size = snapshots.size();
    
    // Collect some stats
    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);
    
    // The snapshots vector should be accessible
    EXPECT_GE(snapshots.size(), initial_size) << "Snapshots vector should be accessible";
}

// ========== AgentStats Class Tests ==========

TEST_F(StatTest, AgentStatsConstructorTest) {
    // AgentStats instance is already created in SetUp
    // Test that we can create another one (though singleton might get overwritten)
    AgentStats another_stats(mock_agent_service_.get());
    SUCCEED();
}

TEST_F(StatTest, AgentStatsWorkerStopTest) {
    // AgentStats instance is already created in SetUp
    
    // Start worker in a separate thread
    std::thread worker_thread([this]() {
        agent_stats_->agentStatsWorker();
    });
    
    // Give worker time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Signal to stop and wait
    mock_agent_service_->setExiting(true);
    agent_stats_->stopAgentStatsWorker();
    
    // Wait for thread to finish
    worker_thread.join();
    
    SUCCEED() << "AgentStats worker should start and stop cleanly";
}

TEST_F(StatTest, AgentStatsWorkerRecordsStatsTest) {
    // AgentStats instance is already created in SetUp
    
    // Start worker in a separate thread
    std::thread worker_thread([this]() {
        agent_stats_->agentStatsWorker();
    });
    
    // Let it run briefly to start up
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Signal to stop and wait
    mock_agent_service_->setExiting(true);
    agent_stats_->stopAgentStatsWorker();
    
    // Wait for thread to finish
    worker_thread.join();
    
    // Check if stats were recorded (may not be called if collection interval hasn't passed)
    // Just verify the test completed without crash
    SUCCEED() << "AgentStats worker should run without crashing";
}

TEST_F(StatTest, AgentStatsWithExitingAgentTest) {
    mock_agent_service_->setExiting(true);
    
    // Re-create stats to pick up exiting state if checked in constructor or init
    // But SetUp already created one. 
    
    // Start worker in a separate thread
    std::thread worker_thread([this]() {
        agent_stats_->agentStatsWorker();
    });
    
    // Let it run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Stop the worker
    agent_stats_->stopAgentStatsWorker();
    
    // Wait for thread to finish
    worker_thread.join();
    
    // When agent is exiting, worker should exit quickly
    SUCCEED() << "AgentStats worker should handle exiting agent gracefully";
}

TEST_F(StatTest, MultipleResponseTimeCollectionTest) {
    // Test collecting multiple response times
    std::vector<int64_t> response_times = {50, 100, 150, 200, 75, 125, 300, 25};
    
    for (int64_t time : response_times) {
        agent_stats_->collectResponseTime(time);
    }
    
    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);
    
    // Verify that max response time is the maximum we inserted
    EXPECT_EQ(snapshot.response_time_max_, 300) << "Max response time should be 300";
    
    // Average should be reasonable (exact value depends on implementation)
    EXPECT_GT(snapshot.response_time_avg_, 0) << "Average response time should be positive";
    EXPECT_LE(snapshot.response_time_avg_, 300) << "Average should not exceed max";
}

TEST_F(StatTest, ActiveSpanTimeDistributionTest) {
    // First, get the current state to see how many spans exist
    AgentStatsSnapshot initial_snapshot;
    agent_stats_->collectAgentStat(initial_snapshot);
    
    int initial_total = initial_snapshot.active_requests_[0] + initial_snapshot.active_requests_[1] + 
                       initial_snapshot.active_requests_[2] + initial_snapshot.active_requests_[3];
    
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    
    // Add spans with different durations using unique IDs
    int64_t base_id = 10000;  // Use higher IDs to avoid conflicts
    agent_stats_->addActiveSpan(base_id + 1, now_ms - 500);   // 500ms old - should be in bucket 0
    agent_stats_->addActiveSpan(base_id + 2, now_ms - 1500);  // 1.5s old - should be in bucket 1
    agent_stats_->addActiveSpan(base_id + 3, now_ms - 3500);  // 3.5s old - should be in bucket 2
    agent_stats_->addActiveSpan(base_id + 4, now_ms - 6000);  // 6s old - should be in bucket 3
    
    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);
    
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
    agent_stats_->dropActiveSpan(base_id + 1);
    agent_stats_->dropActiveSpan(base_id + 2);
    agent_stats_->dropActiveSpan(base_id + 3);
    agent_stats_->dropActiveSpan(base_id + 4);
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

// ========== Additional Edge Case Tests ==========

// Test resetAgentStats clears all counters
TEST_F(StatTest, ResetAgentStatsTest) {
    agent_stats_->incrSampleNew();
    agent_stats_->incrSampleNew();
    agent_stats_->incrUnsampleNew();
    agent_stats_->incrSampleCont();
    agent_stats_->incrUnsampleCont();
    agent_stats_->incrSkipNew();
    agent_stats_->incrSkipCont();
    agent_stats_->collectResponseTime(500);

    agent_stats_->resetAgentStats();

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    EXPECT_EQ(snapshot.num_sample_new_, 0);
    EXPECT_EQ(snapshot.num_unsample_new_, 0);
    EXPECT_EQ(snapshot.num_sample_cont_, 0);
    EXPECT_EQ(snapshot.num_unsample_cont_, 0);
    EXPECT_EQ(snapshot.num_skip_new_, 0);
    EXPECT_EQ(snapshot.num_skip_cont_, 0);
    EXPECT_EQ(snapshot.response_time_avg_, 0);
    EXPECT_EQ(snapshot.response_time_max_, 0);
}

// Test collectAgentStat resets counters between calls
TEST_F(StatTest, CollectResetsCountersBetweenCallsTest) {
    agent_stats_->incrSampleNew();
    agent_stats_->incrSampleNew();
    agent_stats_->incrSampleNew();
    agent_stats_->collectResponseTime(100);
    agent_stats_->collectResponseTime(200);

    AgentStatsSnapshot snapshot1;
    agent_stats_->collectAgentStat(snapshot1);

    EXPECT_EQ(snapshot1.num_sample_new_, 3);
    EXPECT_EQ(snapshot1.response_time_max_, 200);

    // Second collect without new data should show zeros
    AgentStatsSnapshot snapshot2;
    agent_stats_->collectAgentStat(snapshot2);

    EXPECT_EQ(snapshot2.num_sample_new_, 0);
    EXPECT_EQ(snapshot2.num_sample_cont_, 0);
    EXPECT_EQ(snapshot2.num_unsample_new_, 0);
    EXPECT_EQ(snapshot2.num_unsample_cont_, 0);
    EXPECT_EQ(snapshot2.num_skip_new_, 0);
    EXPECT_EQ(snapshot2.num_skip_cont_, 0);
    EXPECT_EQ(snapshot2.response_time_avg_, 0);
    EXPECT_EQ(snapshot2.response_time_max_, 0);
}

// Test single response time
TEST_F(StatTest, SingleResponseTimeTest) {
    agent_stats_->collectResponseTime(42);

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    EXPECT_EQ(snapshot.response_time_avg_, 42);
    EXPECT_EQ(snapshot.response_time_max_, 42);
}

// Test zero response time
TEST_F(StatTest, ZeroResponseTimeTest) {
    agent_stats_->collectResponseTime(0);

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    EXPECT_EQ(snapshot.response_time_avg_, 0);
    EXPECT_EQ(snapshot.response_time_max_, 0);
}

// Test no response times collected (avg should be 0)
TEST_F(StatTest, NoResponseTimesTest) {
    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    EXPECT_EQ(snapshot.response_time_avg_, 0);
    EXPECT_EQ(snapshot.response_time_max_, 0);
}

// Test large response time values
TEST_F(StatTest, LargeResponseTimeTest) {
    int64_t large_time = 1000000000LL; // 1 billion ms
    agent_stats_->collectResponseTime(large_time);

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    EXPECT_EQ(snapshot.response_time_avg_, large_time);
    EXPECT_EQ(snapshot.response_time_max_, large_time);
}

// Test response time average calculation
TEST_F(StatTest, ResponseTimeAverageCalculationTest) {
    agent_stats_->collectResponseTime(100);
    agent_stats_->collectResponseTime(200);
    agent_stats_->collectResponseTime(300);

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    // (100 + 200 + 300) / 3 = 200
    EXPECT_EQ(snapshot.response_time_avg_, 200);
    EXPECT_EQ(snapshot.response_time_max_, 300);
}

// Test dropping a non-existent span ID (should not crash)
TEST_F(StatTest, DropNonExistentSpanTest) {
    agent_stats_->dropActiveSpan(99999);
    SUCCEED();
}

// Test adding duplicate span ID (insert behavior)
TEST_F(StatTest, DuplicateSpanIdTest) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    agent_stats_->addActiveSpan(100, now_ms);
    agent_stats_->addActiveSpan(100, now_ms - 5000); // Same ID, different start time

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    // std::unordered_map::insert does NOT overwrite existing key,
    // so only 1 entry should exist
    int total = snapshot.active_requests_[0] + snapshot.active_requests_[1] +
                snapshot.active_requests_[2] + snapshot.active_requests_[3];
    EXPECT_EQ(total, 1) << "Duplicate spanId insert should keep original entry";

    agent_stats_->dropActiveSpan(100);
}

// Test active request bucket boundary values
TEST_F(StatTest, ActiveRequestBucketBoundariesTest) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Bucket 0: < 1000ms
    agent_stats_->addActiveSpan(1001, now_ms - 999);
    // Bucket 1: >= 1000ms and < 3000ms (exactly at boundary)
    agent_stats_->addActiveSpan(1002, now_ms - 1000);
    // Bucket 2: >= 3000ms and < 5000ms (exactly at boundary)
    agent_stats_->addActiveSpan(1003, now_ms - 3000);
    // Bucket 3: >= 5000ms (exactly at boundary)
    agent_stats_->addActiveSpan(1004, now_ms - 5000);

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    // Due to time passing between addActiveSpan and collectAgentStat,
    // boundary spans might shift up a bucket. We verify total count is correct.
    int total = snapshot.active_requests_[0] + snapshot.active_requests_[1] +
                snapshot.active_requests_[2] + snapshot.active_requests_[3];
    EXPECT_EQ(total, 4) << "All 4 spans should be accounted for";

    // The 999ms span should be in bucket 0 or 1 (timing variance)
    // The 5000ms span should be in bucket 3
    EXPECT_GE(snapshot.active_requests_[3], 1) << "5s+ span should be in last bucket";

    agent_stats_->dropActiveSpan(1001);
    agent_stats_->dropActiveSpan(1002);
    agent_stats_->dropActiveSpan(1003);
    agent_stats_->dropActiveSpan(1004);
}

// Test all active spans in a single bucket
TEST_F(StatTest, AllSpansInOneBucketTest) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // All spans are very recent (< 1s)
    for (int i = 0; i < 5; i++) {
        agent_stats_->addActiveSpan(2000 + i, now_ms - 10);
    }

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    EXPECT_EQ(snapshot.active_requests_[0], 5) << "All spans should be in bucket 0";
    EXPECT_EQ(snapshot.active_requests_[1], 0);
    EXPECT_EQ(snapshot.active_requests_[2], 0);
    EXPECT_EQ(snapshot.active_requests_[3], 0);

    for (int i = 0; i < 5; i++) {
        agent_stats_->dropActiveSpan(2000 + i);
    }
}

// Test empty active span map
TEST_F(StatTest, EmptyActiveSpanMapTest) {
    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(snapshot.active_requests_[i], 0);
    }
}

// Test add and then drop all spans
TEST_F(StatTest, AddAndDropAllSpansTest) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    agent_stats_->addActiveSpan(3001, now_ms);
    agent_stats_->addActiveSpan(3002, now_ms);
    agent_stats_->addActiveSpan(3003, now_ms);

    agent_stats_->dropActiveSpan(3001);
    agent_stats_->dropActiveSpan(3002);
    agent_stats_->dropActiveSpan(3003);

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    int total = snapshot.active_requests_[0] + snapshot.active_requests_[1] +
                snapshot.active_requests_[2] + snapshot.active_requests_[3];
    EXPECT_EQ(total, 0) << "All spans dropped, should have 0 active";
}

// Test concurrent sampling counter increments
TEST_F(StatTest, ConcurrentSamplingCounterTest) {
    const int increments_per_thread = 1000;
    const int num_threads = 4;

    auto increment_fn = [this]() {
        for (int i = 0; i < increments_per_thread; i++) {
            agent_stats_->incrSampleNew();
            agent_stats_->incrUnsampleNew();
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back(increment_fn);
    }
    for (auto& t : threads) {
        t.join();
    }

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    EXPECT_EQ(snapshot.num_sample_new_, num_threads * increments_per_thread);
    EXPECT_EQ(snapshot.num_unsample_new_, num_threads * increments_per_thread);
}

// Test concurrent response time collection
TEST_F(StatTest, ConcurrentResponseTimeTest) {
    const int count_per_thread = 100;
    const int num_threads = 4;

    auto collect_fn = [this]() {
        for (int i = 0; i < count_per_thread; i++) {
            agent_stats_->collectResponseTime(10);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back(collect_fn);
    }
    for (auto& t : threads) {
        t.join();
    }

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    // Total requests = num_threads * count_per_thread, each 10ms
    EXPECT_EQ(snapshot.response_time_avg_, 10);
    EXPECT_EQ(snapshot.response_time_max_, 10);
}

// Test sample_time is set to current time in milliseconds
TEST_F(StatTest, SampleTimeIsCurrentTest) {
    auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    auto after = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    EXPECT_GE(snapshot.sample_time_, before);
    EXPECT_LE(snapshot.sample_time_, after);
}

// Test AgentStatsSnapshot default initialization
TEST_F(StatTest, SnapshotDefaultInitTest) {
    AgentStatsSnapshot snapshot;

    EXPECT_EQ(snapshot.sample_time_, 0);
    EXPECT_DOUBLE_EQ(snapshot.system_cpu_time_, 0.0);
    EXPECT_DOUBLE_EQ(snapshot.process_cpu_time_, 0.0);
    EXPECT_EQ(snapshot.num_threads_, 0);
    EXPECT_EQ(snapshot.heap_alloc_size_, 0);
    EXPECT_EQ(snapshot.heap_max_size_, 0);
    EXPECT_EQ(snapshot.response_time_avg_, 0);
    EXPECT_EQ(snapshot.response_time_max_, 0);
    EXPECT_EQ(snapshot.num_sample_new_, 0);
    EXPECT_EQ(snapshot.num_sample_cont_, 0);
    EXPECT_EQ(snapshot.num_unsample_new_, 0);
    EXPECT_EQ(snapshot.num_unsample_cont_, 0);
    EXPECT_EQ(snapshot.num_skip_new_, 0);
    EXPECT_EQ(snapshot.num_skip_cont_, 0);
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(snapshot.active_requests_[i], 0);
    }
}

// Test initAgentStats resets batch counter
TEST_F(StatTest, InitAgentStatsResetsBatchTest) {
    // Collect some stats first
    agent_stats_->incrSampleNew();
    agent_stats_->collectResponseTime(100);

    // Re-init should reset everything
    agent_stats_->initAgentStats();

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    // After re-init, sampling counters should be 0
    EXPECT_EQ(snapshot.num_sample_new_, 0);
    EXPECT_EQ(snapshot.response_time_avg_, 0);
    EXPECT_EQ(snapshot.response_time_max_, 0);
}

// Test mixed increment patterns across all counters
TEST_F(StatTest, AllCountersMixedIncrementTest) {
    for (int i = 0; i < 10; i++) agent_stats_->incrSampleNew();
    for (int i = 0; i < 20; i++) agent_stats_->incrUnsampleNew();
    for (int i = 0; i < 30; i++) agent_stats_->incrSampleCont();
    for (int i = 0; i < 40; i++) agent_stats_->incrUnsampleCont();
    for (int i = 0; i < 50; i++) agent_stats_->incrSkipNew();
    for (int i = 0; i < 60; i++) agent_stats_->incrSkipCont();

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    EXPECT_EQ(snapshot.num_sample_new_, 10);
    EXPECT_EQ(snapshot.num_unsample_new_, 20);
    EXPECT_EQ(snapshot.num_sample_cont_, 30);
    EXPECT_EQ(snapshot.num_unsample_cont_, 40);
    EXPECT_EQ(snapshot.num_skip_new_, 50);
    EXPECT_EQ(snapshot.num_skip_cont_, 60);
}

// Test getSnapshots returns modifiable reference
TEST_F(StatTest, GetSnapshotsModifiableTest) {
    auto& snapshots = agent_stats_->getSnapshots();
    size_t original_size = snapshots.size();

    snapshots.resize(original_size + 3);
    EXPECT_EQ(agent_stats_->getSnapshots().size(), original_size + 3);
}

// Test many active spans
TEST_F(StatTest, ManyActiveSpansTest) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const int span_count = 100;
    for (int i = 0; i < span_count; i++) {
        agent_stats_->addActiveSpan(5000 + i, now_ms - 100);
    }

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    int total = snapshot.active_requests_[0] + snapshot.active_requests_[1] +
                snapshot.active_requests_[2] + snapshot.active_requests_[3];
    EXPECT_EQ(total, span_count);

    for (int i = 0; i < span_count; i++) {
        agent_stats_->dropActiveSpan(5000 + i);
    }
}

} // namespace pinpoint
