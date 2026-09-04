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
#include <string_view>
#include <vector>

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

// The CPU-load rewrite computes a load only when a prior baseline exists and
// clamps the result to [0,1]; the fix prevents a transient read glitch (or the
// old ms-vs-s period bug) from reporting a bogus > 100% spike. CollectAgentStatTest
// asserts only the lower bound (>= 0 or NaN); this pins the upper clamp, and does
// two sequential collects so the second exercises the baseline/delta path.
TEST_F(StatTest, CpuLoadStaysBoundedAcrossSequentialCollectsTest) {
    auto in_unit_range = [](double v) { return std::isnan(v) || (v >= 0.0 && v <= 1.0); };

    AgentStatsSnapshot first;
    agent_stats_->collectAgentStat(first);
    EXPECT_TRUE(in_unit_range(first.system_cpu_time_)) << "system cpu load must be in [0,1] or NaN";
    EXPECT_TRUE(in_unit_range(first.process_cpu_time_)) << "process cpu load must be in [0,1] or NaN";

    // A little work so the second collect sees a non-trivial delta over a baseline.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    AgentStatsSnapshot second;
    agent_stats_->collectAgentStat(second);
    EXPECT_TRUE(in_unit_range(second.system_cpu_time_))
        << "system cpu load must stay clamped to [0,1] on the delta path, got "
        << second.system_cpu_time_;
    EXPECT_TRUE(in_unit_range(second.process_cpu_time_))
        << "process cpu load must stay clamped to [0,1] on the delta path, got "
        << second.process_cpu_time_;
}

// The Linux reader maps VmHWM (peak *resident* set) onto heap_max, not VmPeak
// (peak *virtual* address space): heap_alloc is VmRSS, so a virtual peak
// reported a max the used value could never approach. /proc does not exist on
// macOS, so the mapping is pinned by feeding the parser sample text directly.
TEST(ProcStatusParseTest, HeapMaxComesFromVmHWMNotVmPeak) {
    // Trimmed /proc/self/status, field order and spacing as the kernel writes it.
    constexpr std::string_view sample =
        "Name:\tpinpoint\n"
        "State:\tS (sleeping)\n"
        "Threads:\t7\n"
        "VmPeak:\t 4194304 kB\n"
        "VmSize:\t 2097152 kB\n"
        "VmHWM:\t   65536 kB\n"
        "VmRSS:\t   32768 kB\n"
        "voluntary_ctxt_switches:\t12\n";

    const auto status = AgentStats::parseProcStatus(sample);

    EXPECT_EQ(status.heap_alloc, 32768LL * 1024) << "heap_alloc is VmRSS in bytes";
    EXPECT_EQ(status.heap_max, 65536LL * 1024) << "heap_max must be VmHWM, not VmPeak";
    EXPECT_EQ(status.num_threads, 7);
}

// Missing fields stay 0 rather than picking up a neighbouring line's value,
// and the last line needs no trailing newline to be parsed.
TEST(ProcStatusParseTest, MissingFieldsStayZero) {
    const auto status = AgentStats::parseProcStatus("Name:\tpinpoint\nVmRSS:\t   1024 kB");

    EXPECT_EQ(status.heap_alloc, 1024LL * 1024);
    EXPECT_EQ(status.heap_max, 0) << "no VmHWM line means no peak reading";
    EXPECT_EQ(status.num_threads, 0);
    EXPECT_EQ(AgentStats::parseProcStatus("").heap_max, 0);
}

// collectInterval must be the gap actually measured between collections, not
// the configured value: the collect timer fires late under load and the
// collector divides the row's counters by whatever interval it is handed
// (Java's CollectJob reports the same measured gap). The first collection has
// no predecessor, so it reports the setting. Driven through the real worker,
// which is where the configured interval is known.
TEST_F(StatTest, CollectIntervalIsMeasuredExceptForTheFirstCollectionTest) {
    constexpr int configured_ms = 200;
    constexpr size_t batch_count = 3;
    auto& config = mock_agent_service_->mutableConfig();
    config->stat.enable = true;
    config->stat.collect_interval = configured_ms;
    config->stat.batch_count = static_cast<int>(batch_count);

    std::thread worker([this] { agent_stats_->agentStatsWorker(); });

    // copySnapshots() stays empty until a batch completes, so the first
    // non-empty read is the worker's first batch — i.e. its first collection
    // is batch[0]. A whole further batch (600ms) would have to elapse between
    // two 10ms polls for that to stop holding.
    std::vector<AgentStatsSnapshot> batch;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        batch = agent_stats_->copySnapshots();
        if (batch.size() == batch_count) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    mock_agent_service_->setExiting(true);
    agent_stats_->stopAgentStatsWorker();
    worker.join();

    ASSERT_EQ(batch.size(), batch_count) << "the worker never completed a batch";
    EXPECT_EQ(batch[0].interval_, configured_ms)
        << "the first collection has no predecessor to measure against";
    for (size_t i = 1; i < batch.size(); i++) {
        // Wide bounds: this pins "measured, not the constant" without making
        // the test depend on the scheduler hitting the interval exactly.
        EXPECT_GE(batch[i].interval_, configured_ms / 2)
            << "snapshot " << i << " interval must reflect the elapsed period";
        EXPECT_LT(batch[i].interval_, configured_ms * 25)
            << "snapshot " << i << " interval is implausibly long";
    }
}

TEST_F(StatTest, CollectResponseTimeTest) {
    struct Case {
        std::vector<int64_t> inputs;
        int64_t expected_avg;
        int64_t expected_max;
    };
    const Case cases[] = {
        {{}, 0, 0},                                        // no response times
        {{0}, 0, 0},                                       // zero response time
        {{42}, 42, 42},                                    // single response time
        {{1000000000LL}, 1000000000LL, 1000000000LL},      // large response time
        {{100, 200, 300}, 200, 300},                       // (100+200+300)/3 = 200
        {{100, 200, 50}, 116, 200},                        // 350/3 truncates to 116
        {{50, 100, 150, 200, 75, 125, 300, 25}, 128, 300}, // 1025/8 truncates to 128
    };

    // collectAgentStat resets the accumulators, so each row starts clean.
    for (const auto& c : cases) {
        std::string inputs = "inputs={";
        for (int64_t time : c.inputs) {
            inputs += std::to_string(time) + ",";
        }
        SCOPED_TRACE(inputs + "}");

        for (int64_t time : c.inputs) {
            agent_stats_->collectResponseTime(time);
        }

        AgentStatsSnapshot snapshot;
        agent_stats_->collectAgentStat(snapshot);

        EXPECT_EQ(snapshot.response_time_avg_, c.expected_avg);
        EXPECT_EQ(snapshot.response_time_max_, c.expected_max);
    }
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

    // Add active spans directly via AgentStats. In production the node is
    // embedded in the span object; local nodes stand in for it here and must
    // be dropped before they go out of scope (see active_span.h).
    ActiveSpanNode node1, node2;
    agent_stats_->addActiveSpan(node1, span_id_1, start_time);
    agent_stats_->addActiveSpan(node2, span_id_2, start_time + 100);

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    // Active spans should be reflected in the snapshot
    // The exact distribution depends on implementation but total should be > 0
    int total_active = snapshot.active_requests_[0] + snapshot.active_requests_[1] +
                      snapshot.active_requests_[2] + snapshot.active_requests_[3];
    EXPECT_GT(total_active, 0) << "Should have active spans";

    // Drop one span
    agent_stats_->dropActiveSpan(node1);

    agent_stats_->collectAgentStat(snapshot);

    // Should have fewer active spans now
    int new_total_active = snapshot.active_requests_[0] + snapshot.active_requests_[1] +
                          snapshot.active_requests_[2] + snapshot.active_requests_[3];
    EXPECT_LE(new_total_active, total_active) << "Should have same or fewer active spans after dropping";

    // A duplicate drop must be a harmless no-op (destructor-backstop path).
    agent_stats_->dropActiveSpan(node1);
    agent_stats_->dropActiveSpan(node2);
}

TEST_F(StatTest, GetAgentStatSnapshotsTest) {
    // Test that we can copy the snapshots batch
    auto snapshots = agent_stats_->copySnapshots();

    size_t initial_size = snapshots.size();

    // Collect some stats
    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    // The snapshots batch should be accessible
    EXPECT_GE(agent_stats_->copySnapshots().size(), initial_size)
        << "Snapshots batch should be accessible";
}

// The gRPC stats stream reads copySnapshots() when it consumes the
// AGENT_STATS token, which a stalled stream can delay well past the next
// collect tick. It must still get the cycle the token was enqueued for:
// handing out the working vector meant that with a 2-slot batch a copy taken
// between two publications was [tick 3, tick 2] — the next cycle's first
// slot in front of the pending cycle's second.
TEST_F(StatTest, CopySnapshotsHoldsCompletedBatchWhileNextCycleCollects) {
    auto& cfg = *mock_agent_service_->mutableConfig();
    cfg.stat.enable = true;
    cfg.stat.collect_interval = 20;  // ms
    cfg.stat.batch_count = 2;

    EXPECT_TRUE(agent_stats_->copySnapshots().empty()) << "no cycle has completed yet";

    std::thread worker_thread([this]() { agent_stats_->agentStatsWorker(); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (mock_agent_service_->recorded_stats_calls_.load() < 1 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto first = agent_stats_->copySnapshots();

    // Poll across the whole window until the second cycle is published: every
    // copy must be a complete cycle, i.e. slots in collection order.
    bool consistent = true;
    while (mock_agent_service_->recorded_stats_calls_.load() < 2 &&
           std::chrono::steady_clock::now() < deadline) {
        const auto copy = agent_stats_->copySnapshots();
        if (copy.size() != 2 || copy[0].sample_time_ > copy[1].sample_time_) {
            consistent = false;
            break;
        }
        std::this_thread::yield();
    }

    mock_agent_service_->setExiting(true);
    agent_stats_->stopAgentStatsWorker();
    worker_thread.join();

    ASSERT_GE(mock_agent_service_->recorded_stats_calls_.load(), 2);
    ASSERT_EQ(first.size(), 2U);
    EXPECT_GT(first[0].sample_time_, 0);
    EXPECT_LE(first[0].sample_time_, first[1].sample_time_);
    EXPECT_TRUE(consistent)
        << "slot 0 was overwritten by the next cycle while its batch was still pending";
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
    
    std::thread worker_thread([this]() {
        agent_stats_->agentStatsWorker();
    });
    
    // Give worker time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    mock_agent_service_->setExiting(true);
    agent_stats_->stopAgentStatsWorker();
    
    worker_thread.join();
    
    SUCCEED() << "AgentStats worker should start and stop cleanly";
}

TEST_F(StatTest, AgentStatsWorkerRecordsStatsTest) {
    // AgentStats instance is already created in SetUp
    
    std::thread worker_thread([this]() {
        agent_stats_->agentStatsWorker();
    });
    
    // Let it run briefly to start up
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    mock_agent_service_->setExiting(true);
    agent_stats_->stopAgentStatsWorker();
    
    worker_thread.join();
    
    // Check if stats were recorded (may not be called if collection interval hasn't passed)
    // Just verify the test completed without crash
    SUCCEED() << "AgentStats worker should run without crashing";
}

TEST_F(StatTest, AgentStatsWorkerSurvivesTransientRecordStatsFailure) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->stat.enable = true;
    cfg->stat.collect_interval = 20;  // ms; production default is seconds-scale
    cfg->stat.batch_count = 1;        // send on every collection

    // The first send throws out of the collect loop; the supervisor must
    // restart the worker (paced by the collect interval) instead of letting
    // one transient failure end agent-stat reporting for the process
    // lifetime.
    mock_agent_service_->stats_throws_remaining_ = 1;

    std::thread worker_thread([this]() { agent_stats_->agentStatsWorker(); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (mock_agent_service_->recorded_stats_calls_.load() < 3 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    mock_agent_service_->setExiting(true);
    agent_stats_->stopAgentStatsWorker();
    worker_thread.join();

    EXPECT_GE(mock_agent_service_->recorded_stats_calls_.load(), 3)
        << "the worker must be restarted after a transient recordStats failure";
}

TEST_F(StatTest, AgentStatsWithExitingAgentTest) {
    mock_agent_service_->setExiting(true);
    
    // Re-create stats to pick up exiting state if checked in constructor or init
    // But SetUp already created one. 
    
    std::thread worker_thread([this]() {
        agent_stats_->agentStatsWorker();
    });
    
    // Let it run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Stop the worker
    agent_stats_->stopAgentStatsWorker();
    
    worker_thread.join();
    
    // When agent is exiting, worker should exit quickly
    SUCCEED() << "AgentStats worker should handle exiting agent gracefully";
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
    ActiveSpanNode nodes[4];
    agent_stats_->addActiveSpan(nodes[0], base_id + 1, now_ms - 500);   // 500ms old - should be in bucket 0
    agent_stats_->addActiveSpan(nodes[1], base_id + 2, now_ms - 1500);  // 1.5s old - should be in bucket 1
    agent_stats_->addActiveSpan(nodes[2], base_id + 3, now_ms - 3500);  // 3.5s old - should be in bucket 2
    agent_stats_->addActiveSpan(nodes[3], base_id + 4, now_ms - 6000);  // 6s old - should be in bucket 3
    
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
    agent_stats_->dropActiveSpan(nodes[0]);
    agent_stats_->dropActiveSpan(nodes[1]);
    agent_stats_->dropActiveSpan(nodes[2]);
    agent_stats_->dropActiveSpan(nodes[3]);
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

// Test dropping a never-linked node (async/noop spans; should not crash)
TEST_F(StatTest, DropNonExistentSpanTest) {
    ActiveSpanNode node;
    agent_stats_->dropActiveSpan(node);
    SUCCEED();
}

// Test adding the same node twice (re-link guard)
TEST_F(StatTest, DuplicateSpanIdTest) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    ActiveSpanNode node;
    agent_stats_->addActiveSpan(node, 100, now_ms);
    agent_stats_->addActiveSpan(node, 100, now_ms - 5000); // Same node again

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    // Re-linking a linked node must be a no-op (a span registers exactly
    // once; the guard turns a contract violation into a kept original entry,
    // the same tolerance the old map's try_emplace gave a duplicate id).
    int total = snapshot.active_requests_[0] + snapshot.active_requests_[1] +
                snapshot.active_requests_[2] + snapshot.active_requests_[3];
    EXPECT_EQ(total, 1) << "Duplicate add of a linked node should keep original entry";

    agent_stats_->dropActiveSpan(node);
}

// Test active request bucket boundary values
TEST_F(StatTest, ActiveRequestBucketBoundariesTest) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    ActiveSpanNode nodes[4];
    // Bucket 0: < 1000ms
    agent_stats_->addActiveSpan(nodes[0], 1001, now_ms - 999);
    // Bucket 1: >= 1000ms and < 3000ms (exactly at boundary)
    agent_stats_->addActiveSpan(nodes[1], 1002, now_ms - 1000);
    // Bucket 2: >= 3000ms and < 5000ms (exactly at boundary)
    agent_stats_->addActiveSpan(nodes[2], 1003, now_ms - 3000);
    // Bucket 3: >= 5000ms (exactly at boundary)
    agent_stats_->addActiveSpan(nodes[3], 1004, now_ms - 5000);

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

    agent_stats_->dropActiveSpan(nodes[0]);
    agent_stats_->dropActiveSpan(nodes[1]);
    agent_stats_->dropActiveSpan(nodes[2]);
    agent_stats_->dropActiveSpan(nodes[3]);
}

// Test all active spans in a single bucket
TEST_F(StatTest, AllSpansInOneBucketTest) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // All spans are very recent (< 1s)
    ActiveSpanNode nodes[5];
    for (int i = 0; i < 5; i++) {
        agent_stats_->addActiveSpan(nodes[i], 2000 + i, now_ms - 10);
    }

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    EXPECT_EQ(snapshot.active_requests_[0], 5) << "All spans should be in bucket 0";
    EXPECT_EQ(snapshot.active_requests_[1], 0);
    EXPECT_EQ(snapshot.active_requests_[2], 0);
    EXPECT_EQ(snapshot.active_requests_[3], 0);

    for (int i = 0; i < 5; i++) {
        agent_stats_->dropActiveSpan(nodes[i]);
    }
}

TEST_F(StatTest, CollectActiveRequestsMatchesHistogramBucketsTest) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    ActiveSpanNode nodes[4];
    agent_stats_->addActiveSpan(nodes[0], 3001, now_ms - 100);
    agent_stats_->addActiveSpan(nodes[1], 3002, now_ms - 1200);
    agent_stats_->addActiveSpan(nodes[2], 3003, now_ms - 3200);
    agent_stats_->addActiveSpan(nodes[3], 3004, now_ms - 5200);

    int32_t active_requests[4]{};
    agent_stats_->collectActiveRequests(active_requests, now_ms);

    EXPECT_EQ(active_requests[0], 1);
    EXPECT_EQ(active_requests[1], 1);
    EXPECT_EQ(active_requests[2], 1);
    EXPECT_EQ(active_requests[3], 1);

    agent_stats_->dropActiveSpan(nodes[0]);
    agent_stats_->dropActiveSpan(nodes[1]);
    agent_stats_->dropActiveSpan(nodes[2]);
    agent_stats_->dropActiveSpan(nodes[3]);
}

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

    ActiveSpanNode nodes[3];
    agent_stats_->addActiveSpan(nodes[0], 3001, now_ms);
    agent_stats_->addActiveSpan(nodes[1], 3002, now_ms);
    agent_stats_->addActiveSpan(nodes[2], 3003, now_ms);

    agent_stats_->dropActiveSpan(nodes[0]);
    agent_stats_->dropActiveSpan(nodes[1]);
    agent_stats_->dropActiveSpan(nodes[2]);

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

// Test concurrent active span collection
TEST_F(StatTest, ConcurrentActiveSpanTest) {
    const int spans_per_thread = 50;
    const int num_threads = 4;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // One node per simulated span, shared by the add and drop phases.
    std::vector<ActiveSpanNode> nodes(num_threads * spans_per_thread);

    auto add_fn = [this, now_ms, &nodes](int thread_index) {
        const int base = thread_index * spans_per_thread;
        for (int i = 0; i < spans_per_thread; i++) {
            agent_stats_->addActiveSpan(nodes[base + i], 100000 + base + i, now_ms - 10);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back(add_fn, t);
    }
    for (auto& t : threads) {
        t.join();
    }

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    int total = snapshot.active_requests_[0] + snapshot.active_requests_[1] +
                snapshot.active_requests_[2] + snapshot.active_requests_[3];
    EXPECT_EQ(total, num_threads * spans_per_thread);

    threads.clear();
    auto drop_fn = [this, &nodes](int thread_index) {
        const int base = thread_index * spans_per_thread;
        for (int i = 0; i < spans_per_thread; i++) {
            agent_stats_->dropActiveSpan(nodes[base + i]);
        }
    };

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back(drop_fn, t);
    }
    for (auto& t : threads) {
        t.join();
    }

    agent_stats_->collectAgentStat(snapshot);
    total = snapshot.active_requests_[0] + snapshot.active_requests_[1] +
            snapshot.active_requests_[2] + snapshot.active_requests_[3];
    EXPECT_EQ(total, 0);
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

TEST_F(StatTest, ManyActiveSpansTest) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const int span_count = 100;
    std::vector<ActiveSpanNode> nodes(span_count);
    for (int i = 0; i < span_count; i++) {
        agent_stats_->addActiveSpan(nodes[i], 5000 + i, now_ms - 100);
    }

    AgentStatsSnapshot snapshot;
    agent_stats_->collectAgentStat(snapshot);

    int total = snapshot.active_requests_[0] + snapshot.active_requests_[1] +
                snapshot.active_requests_[2] + snapshot.active_requests_[3];
    EXPECT_EQ(total, span_count);

    for (int i = 0; i < span_count; i++) {
        agent_stats_->dropActiveSpan(nodes[i]);
    }
}

} // namespace pinpoint
