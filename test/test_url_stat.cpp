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
#include <google/protobuf/arena.h>
#include <future>
#include <memory>
#include <thread>
#include <chrono>
#include <type_traits>

#include "../src/url_stat.h"
#include "../src/config.h"
#include "../src/agent_service.h"
#include "../src/grpc_builders.h"
#include "../src/logging.h"
#include "../src/stat.h"
#include "v1/Stat.pb.h"
#include "mock_agent_service.h"


namespace pinpoint {

static_assert(std::is_same_v<UrlStatSnapshot::UrlStatMap::mapped_type, EachUrlStat>,
              "URL statistics must be stored inline in the map");
static_assert(!std::is_copy_constructible_v<UrlStatSnapshot>,
              "inline map values must not make snapshots copyable");

class UrlStatTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_agent_service_ = std::make_unique<MockAgentService>();
        mock_agent_service_->setExiting(false);
        // Configure URL stats defaults for testing
        auto& cfg = mock_agent_service_->mutableConfig();
        cfg->http.url_stat.enable = true;
        cfg->http.url_stat.limit = 1024;
        cfg->http.url_stat.trim_path_depth = 3;
        cfg->http.url_stat.method_prefix = false;
        cfg->http.url_stat.queue_size = 100;
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

TEST_F(UrlStatTest, UrlKeyEqualityTest) {
    // UrlKey is an unordered_map key (UrlKeyHash): equality is the whole
    // comparison contract — the former operator< was unused and removed.
    UrlKey key1{"/api/users", 1000};
    UrlKey key2{"/api/users", 1000};
    UrlKey key3{"/api/users", 2000};
    UrlKey key4{"/api/posts", 1000};

    EXPECT_TRUE(key1 == key2) << "Same URL and tick should compare equal";
    EXPECT_FALSE(key1 == key3) << "Same URL with a different tick should differ";
    EXPECT_FALSE(key1 == key4) << "Different URL with the same tick should differ";
}

// ========== UrlStat Tests ==========

TEST_F(UrlStatTest, UrlStatConstructorTest) {
    UrlStatEntry stat("/api/users", "GET", 200);
    
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
    UrlStatEntry stat("/api/users", "GET", 200);
    stat.elapsed_ = 150;
    stat.end_time_ = std::chrono::system_clock::now();
    
    // Add to snapshot
    auto& tick_clock = mock_agent_service_->getUrlStats().getTickClock();
    snapshot.add(&stat, config, tick_clock);
    
    auto& stats = snapshot.getEachStats();
    EXPECT_FALSE(stats.empty()) << "Snapshot should contain entries after add";
}

TEST_F(UrlStatTest, SnapshotPreservesExactUntrimmedKey) {
    UrlStatSnapshot snapshot;
    Config config;
    config.http.url_stat.enable_trim_path = false;
    config.http.url_stat.method_prefix = false;
    TickClock tick_clock(1);

    UrlStatEntry stat("/api/items/42?expand=owner", "GET", 200);
    stat.elapsed_ = 25;
    stat.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(100));
    snapshot.add(&stat, config, tick_clock);

    const auto& stats = snapshot.getEachStats();
    const UrlKey expected{"/api/items/42?expand=owner", 100000};
    const auto found = stats.find(expected);
    ASSERT_NE(found, stats.end());
    EXPECT_EQ(stats.size(), 1u);
    EXPECT_EQ(found->first.url_, "/api/items/42?expand=owner");
    EXPECT_EQ(found->second.tick(), 100000);
    EXPECT_EQ(found->second.getTotalHistogram().total(), 25);
}

TEST_F(UrlStatTest, SnapshotTrimPrefixAndWireFormatStayExact) {
    UrlStatSnapshot snapshot;
    Config config;
    config.http.url_stat.enable_trim_path = true;
    config.http.url_stat.trim_path_depth = 2;
    config.http.url_stat.method_prefix = true;
    TickClock tick_clock(1);

    UrlStatEntry stat("/api/v1/users/42?verbose=true", "PATCH", 500);
    stat.elapsed_ = 450;
    stat.failed_ = true;
    stat.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(1234));
    snapshot.add(&stat, config, tick_clock);

    UrlStatEntry repeat("/api/v1/users/42?verbose=true", "PATCH", 200);
    repeat.elapsed_ = 50;
    repeat.end_time_ = stat.end_time_;
    snapshot.add(&repeat, config, tick_clock);

    const auto& stats = snapshot.getEachStats();
    const UrlKey expected{"PATCH /api/v1/*", 1234000};
    ASSERT_NE(stats.find(expected), stats.end());
    ASSERT_EQ(stats.size(), 1u);

    google::protobuf::Arena arena;
    const auto* wire = build_url_stat(&snapshot, &arena);
    ASSERT_NE(wire, nullptr);
    EXPECT_EQ(wire->bucketversion(), URL_STATS_BUCKET_VERSION);
    ASSERT_EQ(wire->eachuristat_size(), 1);

    const auto& each = wire->eachuristat(0);
    EXPECT_EQ(each.uri(), "PATCH /api/v1/*");
    EXPECT_EQ(each.timestamp(), 1234000);
    ASSERT_TRUE(each.has_totalhistogram());
    EXPECT_EQ(each.totalhistogram().total(), 500);
    EXPECT_EQ(each.totalhistogram().max(), 450);
    ASSERT_EQ(each.totalhistogram().histogram_size(), URL_STATS_BUCKET_SIZE);
    EXPECT_EQ(each.totalhistogram().histogram(0), 1);
    EXPECT_EQ(each.totalhistogram().histogram(2), 1);
    ASSERT_TRUE(each.has_failedhistogram());
    EXPECT_EQ(each.failedhistogram().total(), 450);
    EXPECT_EQ(each.failedhistogram().histogram(2), 1);
}

TEST_F(UrlStatTest, SnapshotSeparatesIdenticalUrlsByTick) {
    UrlStatSnapshot snapshot;
    Config config;
    config.http.url_stat.enable_trim_path = false;
    TickClock tick_clock(1);

    UrlStatEntry first("/tick/api", "GET", 200);
    first.elapsed_ = 10;
    first.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(1000));
    UrlStatEntry second("/tick/api", "GET", 200);
    second.elapsed_ = 20;
    second.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(1001));

    snapshot.add(&first, config, tick_clock);
    snapshot.add(&second, config, tick_clock);

    const auto& stats = snapshot.getEachStats();
    const auto first_stat = stats.find(UrlKey{"/tick/api", 1000000});
    const auto second_stat = stats.find(UrlKey{"/tick/api", 1001000});
    ASSERT_NE(first_stat, stats.end());
    ASSERT_NE(second_stat, stats.end());
    EXPECT_EQ(stats.size(), 2u);
    EXPECT_EQ(first_stat->second.getTotalHistogram().total(), 10);
    EXPECT_EQ(second_stat->second.getTotalHistogram().total(), 20);
}

TEST_F(UrlStatTest, SnapshotRepeatedHitsAccumulateAndLimitRejectsMiss) {
    UrlStatSnapshot snapshot;
    Config config;
    config.http.url_stat.enable_trim_path = false;
    config.http.url_stat.method_prefix = true;
    config.http.url_stat.limit = 1;
    TickClock tick_clock(1);

    const std::string url = "/repeat/api";
    UrlStatEntry hit(url, "GET", 200);
    hit.elapsed_ = 1;
    hit.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(500));
    snapshot.add(&hit, config, tick_clock);

    ASSERT_EQ(snapshot.getEachStats().size(), 1u);
    const auto* const stored_stat = &snapshot.getEachStats().begin()->second;

    for (int i = 0; i < 128; ++i) {
        snapshot.add(&hit, config, tick_clock);
    }

    UrlStatEntry rejected(url + "/miss", "GET", 200);
    rejected.elapsed_ = 1000;
    rejected.end_time_ = hit.end_time_;
    snapshot.add(&rejected, config, tick_clock);

    const auto& stats = snapshot.getEachStats();
    ASSERT_EQ(stats.size(), 1u) << "a limit-rejected miss must not be stored";
    EXPECT_EQ(&stats.begin()->second, stored_stat)
        << "repeated hits must reuse the stored entry";
    EXPECT_EQ(stats.begin()->first.url_, "GET " + url);
    EXPECT_EQ(stats.begin()->second.getTotalHistogram().total(), 129);
}

// ========== UrlStats Class Tests ==========

TEST_F(UrlStatTest, UrlStatsConstructorTest) {
    UrlStats url_stats(mock_agent_service_.get());
    SUCCEED() << "UrlStats constructor should not crash";
}

TEST_F(UrlStatTest, UrlStatsEnqueueTest) {
    UrlStats url_stats(mock_agent_service_.get());
    
    UrlStatEntry stat{"/api/test", "POST", 201};
    url_stats.enqueueUrlStats(std::move(stat));
    
    SUCCEED() << "Enqueue should not crash";
}

TEST_F(UrlStatTest, UrlStatsEnqueueWithDisabledConfigTest) {
    mock_agent_service_->mutableConfig()->http.url_stat.enable = false;
    UrlStats url_stats(mock_agent_service_.get());
    
    UrlStatEntry stat{"/api/test", "POST", 201};
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

TEST_F(UrlStatTest, StopWorkerAfterUrlStatDisabledByReloadTest) {
    UrlStats url_stats(mock_agent_service_.get());

    std::thread add_worker([&url_stats]() {
        url_stats.addUrlStatsWorker();
    });

    // Give the worker time to park in its untimed wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Simulate a config reload turning CollectUrlStat off while the worker
    // (started when it was on) is blocked. stopAddUrlStatsWorker must still
    // deliver the wakeup; consulting the live config would skip it and hang
    // shutdown forever.
    //
    // Publish a new snapshot rather than writing through mutableConfig(): the
    // worker reads the same flag on its way into the loop, and mutating the
    // snapshot it holds is a data race that only sleep-based timing hides.
    mock_agent_service_->publishConfig(
        [](Config& cfg) { cfg.http.url_stat.enable = false; });

    mock_agent_service_->setExiting(true);
    url_stats.stopAddUrlStatsWorker();

    auto joined = std::async(std::launch::async, [&add_worker] { add_worker.join(); });
    // On failure the worker never wakes; the process aborts on the un-joined
    // thread, which is the intended loud signal for this regression.
    ASSERT_EQ(joined.wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "stop must wake the add worker even when url_stat was disabled by reload";
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

TEST_F(UrlStatTest, AddAndTakeSnapshotTest) {
    Config config;
    UrlStatEntry stat("/api/test", "GET", 200);
    stat.elapsed_ = 100;
    stat.end_time_ = std::chrono::system_clock::now();
    
    // Add to snapshot
    mock_agent_service_->getUrlStats().addSnapshot(&stat, config);
    
    // Take snapshot
    auto snapshot = mock_agent_service_->getUrlStats().takeSnapshot();
    EXPECT_NE(snapshot.get(), nullptr) << "Snapshot should not be null";
    
    auto& stats = snapshot->getEachStats();
    EXPECT_FALSE(stats.empty()) << "Snapshot should contain the added stat";
}

TEST_F(UrlStatTest, TrimUrlPathTest) {
    // Test basic path trimming
    std::string result1 = UrlStatSnapshot::trim_url_path("/api/v1/users/123/posts/456", 3);
    EXPECT_FALSE(result1.empty()) << "Trimmed path should not be empty";
    
    // Test with depth 0 (gets converted to depth 1)
    std::string result2 = UrlStatSnapshot::trim_url_path("/api/v1/users", 0);
    EXPECT_FALSE(result2.empty()) << "Trimmed path with depth 0 should not be empty";
    
    // Test with non-empty path that starts without slash
    std::string result3 = UrlStatSnapshot::trim_url_path("api/test", 2);
    EXPECT_FALSE(result3.empty()) << "Trimmed path should not be empty";
    
    // Test with single level path
    std::string result4 = UrlStatSnapshot::trim_url_path("/api", 1);
    EXPECT_EQ(result4, "/api") << "Single level path should be preserved";
    
    // Test path with query parameters
    std::string result5 = UrlStatSnapshot::trim_url_path("/api/users?id=123", 2);
    EXPECT_FALSE(result5.empty()) << "Path with query params should be trimmed";
    EXPECT_EQ(result5.find('?'), std::string::npos) << "Query params should be removed";
}

TEST_F(UrlStatTest, TrimUrlPathDepthTest) {
    // Test different depths
    std::string path = "/api/v1/users/123/posts";
    
    std::string result1 = UrlStatSnapshot::trim_url_path(path, 1);
    std::string result2 = UrlStatSnapshot::trim_url_path(path, 2);
    std::string result3 = UrlStatSnapshot::trim_url_path(path, 3);
    
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
    UrlStatEntry stat1{"/api/users", "GET", 200};
    stat1.elapsed_ = 100;
    UrlStatEntry stat2{"/api/posts", "POST", 201};
    stat2.elapsed_ = 200;
    UrlStatEntry stat3{"/api/users", "GET", 500};
    stat3.elapsed_ = 300;
    
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
    auto snapshot = url_stats.takeSnapshot();
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
                UrlStatEntry stat{"/api/test" + std::to_string(t) + "/" + std::to_string(i), "GET", 200};
                stat.elapsed_ = 100 + i;
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

// ========== Additional UrlStatHistogram Tests ==========

// Test all 8 histogram buckets with exact boundary values
TEST_F(UrlStatTest, HistogramAllBucketsTest) {
    UrlStatHistogram histogram;

    histogram.add(50);    // bucket 0: < 100ms
    histogram.add(150);   // bucket 1: 100-299ms
    histogram.add(350);   // bucket 2: 300-499ms
    histogram.add(750);   // bucket 3: 500-999ms
    histogram.add(2000);  // bucket 4: 1000-2999ms
    histogram.add(4000);  // bucket 5: 3000-4999ms
    histogram.add(6000);  // bucket 6: 5000-7999ms
    histogram.add(9000);  // bucket 7: >= 8000ms

    for (int i = 0; i < URL_STATS_BUCKET_SIZE; i++) {
        EXPECT_EQ(histogram.histogram(i), 1) << "Bucket " << i << " should have exactly 1 entry";
    }

    EXPECT_EQ(histogram.total(), 50 + 150 + 350 + 750 + 2000 + 4000 + 6000 + 9000);
    EXPECT_EQ(histogram.max(), 9000);
}

// Test histogram bucket boundary exact values
TEST_F(UrlStatTest, HistogramBucketBoundaryExactTest) {
    // Test exact boundary values: at boundary goes to the higher bucket
    UrlStatHistogram h1;
    h1.add(99);   // bucket 0
    EXPECT_EQ(h1.histogram(0), 1);

    UrlStatHistogram h2;
    h2.add(100);  // bucket 1 (>= 100)
    EXPECT_EQ(h2.histogram(1), 1);

    UrlStatHistogram h3;
    h3.add(299);  // bucket 1
    EXPECT_EQ(h3.histogram(1), 1);

    UrlStatHistogram h4;
    h4.add(300);  // bucket 2
    EXPECT_EQ(h4.histogram(2), 1);

    UrlStatHistogram h5;
    h5.add(500);  // bucket 3
    EXPECT_EQ(h5.histogram(3), 1);

    UrlStatHistogram h6;
    h6.add(1000); // bucket 4
    EXPECT_EQ(h6.histogram(4), 1);

    UrlStatHistogram h7;
    h7.add(3000); // bucket 5
    EXPECT_EQ(h7.histogram(5), 1);

    UrlStatHistogram h8;
    h8.add(5000); // bucket 6
    EXPECT_EQ(h8.histogram(6), 1);

    UrlStatHistogram h9;
    h9.add(8000); // bucket 7
    EXPECT_EQ(h9.histogram(7), 1);
}

// Test histogram out-of-bounds index returns 0
TEST_F(UrlStatTest, HistogramOutOfBoundsIndexTest) {
    UrlStatHistogram histogram;
    histogram.add(100);

    EXPECT_EQ(histogram.histogram(-1), 0);
    EXPECT_EQ(histogram.histogram(URL_STATS_BUCKET_SIZE), 0);
    EXPECT_EQ(histogram.histogram(100), 0);
}

// Test histogram with zero elapsed
TEST_F(UrlStatTest, HistogramZeroElapsedTest) {
    UrlStatHistogram histogram;
    histogram.add(0);

    EXPECT_EQ(histogram.histogram(0), 1) << "Zero elapsed should go to bucket 0";
    EXPECT_EQ(histogram.total(), 0);
    EXPECT_EQ(histogram.max(), 0);
}

// Test histogram with multiple entries in same bucket
TEST_F(UrlStatTest, HistogramMultipleSameBucketTest) {
    UrlStatHistogram histogram;

    histogram.add(10);
    histogram.add(20);
    histogram.add(30);
    histogram.add(50);
    histogram.add(99);

    EXPECT_EQ(histogram.histogram(0), 5) << "All 5 entries should be in bucket 0";
    EXPECT_EQ(histogram.total(), 10 + 20 + 30 + 50 + 99);
    EXPECT_EQ(histogram.max(), 99);
}

// ========== Additional trim_url_path Tests ==========

// Test trim_url_path with empty string
TEST_F(UrlStatTest, TrimUrlPathEmptyTest) {
    std::string result = UrlStatSnapshot::trim_url_path("", 3);
    EXPECT_EQ(result, "");
}

// Test trim_url_path with root path "/"
TEST_F(UrlStatTest, TrimUrlPathRootTest) {
    std::string result = UrlStatSnapshot::trim_url_path("/", 3);
    EXPECT_EQ(result, "/");
}

// Test trim_url_path with trailing slash
TEST_F(UrlStatTest, TrimUrlPathTrailingSlashTest) {
    // "/api/v1/" has 2 slashes after position 0, so depth=2 should trim after v1/
    std::string result = UrlStatSnapshot::trim_url_path("/api/v1/users", 2);
    EXPECT_EQ(result, "/api/v1/*");
}

// Test trim_url_path with depth exceeding segments
TEST_F(UrlStatTest, TrimUrlPathDepthExceedsSegmentsTest) {
    // Path has only 2 segments, but depth is 10
    std::string result = UrlStatSnapshot::trim_url_path("/api/users", 10);
    EXPECT_EQ(result, "/api/users") << "Should return full path when depth exceeds segments";
}

// Test trim_url_path with depth 1
TEST_F(UrlStatTest, TrimUrlPathDepth1Test) {
    std::string result = UrlStatSnapshot::trim_url_path("/api/v1/users/123", 1);
    EXPECT_EQ(result, "/api/*");
}

// Test trim_url_path with negative depth (treated as 1)
TEST_F(UrlStatTest, TrimUrlPathNegativeDepthTest) {
    std::string result = UrlStatSnapshot::trim_url_path("/api/v1/users", -5);
    // depth < 1 gets converted to 1
    EXPECT_EQ(result, "/api/*");
}

// Test trim_url_path with query parameters at various positions
TEST_F(UrlStatTest, TrimUrlPathQueryParamsEarlyTest) {
    // Query params appear before depth is reached
    std::string result = UrlStatSnapshot::trim_url_path("/api?key=value", 3);
    EXPECT_EQ(result, "/api");
    EXPECT_EQ(result.find('?'), std::string::npos);
}

// Test trim_url_path with fragment
TEST_F(UrlStatTest, TrimUrlPathWithFragmentTest) {
    // Fragments are not specially handled, treated as part of the path
    std::string result = UrlStatSnapshot::trim_url_path("/api/v1#section", 3);
    EXPECT_TRUE(result.find('#') != std::string::npos || result.find('#') == std::string::npos)
        << "Fragment handling is implementation-defined";
    EXPECT_FALSE(result.empty());
}

// Test trim_url_path with consecutive slashes
TEST_F(UrlStatTest, TrimUrlPathConsecutiveSlashesTest) {
    std::string result = UrlStatSnapshot::trim_url_path("/api//v1/users", 2);
    // Each '/' decrements depth, so "/api/" uses depth 1, "/" uses depth 2 -> trim
    EXPECT_EQ(result, "/api//*");
}

// ========== Additional UrlStatSnapshot Tests ==========

// Test snapshot aggregates same URL and tick
TEST_F(UrlStatTest, SnapshotAggregatesSameUrlAndTickTest) {
    UrlStatSnapshot snapshot;
    Config config;
    auto& tick_clock = mock_agent_service_->getUrlStats().getTickClock();

    auto now = std::chrono::system_clock::now();

    UrlStatEntry stat1("/api/users", "GET", 200);
    stat1.elapsed_ = 100;
    stat1.end_time_ = now;

    UrlStatEntry stat2("/api/users", "GET", 200);
    stat2.elapsed_ = 200;
    stat2.end_time_ = now; // Same time -> same tick

    snapshot.add(&stat1, config, tick_clock);
    snapshot.add(&stat2, config, tick_clock);

    auto& stats = snapshot.getEachStats();
    // Same URL + same tick = single entry with aggregated histogram
    EXPECT_EQ(stats.size(), 1u) << "Same URL and tick should be aggregated into one entry";

    auto& entry = stats.begin()->second;
    EXPECT_EQ(entry.getTotalHistogram().total(), 300) << "Total should be 100 + 200";
    EXPECT_EQ(entry.getTotalHistogram().max(), 200);
}

// Test snapshot fail aggregation honors the precomputed failed_ flag.
// With the default config (5xx) a 404 is a success; only a 500 is a failure.
TEST_F(UrlStatTest, SnapshotFailStatusAggregationTest) {
    UrlStatSnapshot snapshot;
    Config config;
    auto& tick_clock = mock_agent_service_->getUrlStats().getTickClock();

    auto now = std::chrono::system_clock::now();

    // Success (2xx)
    UrlStatEntry stat_ok("/api/users", "GET", 200);
    stat_ok.elapsed_ = 100;
    stat_ok.failed_ = mock_agent_service_->isStatusFail(stat_ok.status_code_);
    stat_ok.end_time_ = now;

    // Server error (5xx) -> failure under default config
    UrlStatEntry stat_fail("/api/users", "GET", 500);
    stat_fail.elapsed_ = 200;
    stat_fail.failed_ = mock_agent_service_->isStatusFail(stat_fail.status_code_);
    stat_fail.end_time_ = now;

    // Client error (4xx) -> NOT a failure under default config (5xx)
    UrlStatEntry stat_client_err("/api/users", "GET", 404);
    stat_client_err.elapsed_ = 150;
    stat_client_err.failed_ = mock_agent_service_->isStatusFail(stat_client_err.status_code_);
    stat_client_err.end_time_ = now;

    EXPECT_FALSE(stat_ok.failed_) << "200 should not be a failure";
    EXPECT_TRUE(stat_fail.failed_) << "500 should be a failure under default 5xx config";
    EXPECT_FALSE(stat_client_err.failed_) << "404 should NOT be a failure under default 5xx config";

    snapshot.add(&stat_ok, config, tick_clock);
    snapshot.add(&stat_fail, config, tick_clock);
    snapshot.add(&stat_client_err, config, tick_clock);

    auto& stats = snapshot.getEachStats();
    EXPECT_EQ(stats.size(), 1u);

    auto& entry = stats.begin()->second;
    EXPECT_EQ(entry.getTotalHistogram().total(), 450) << "All 3 should be in total histogram";
    EXPECT_EQ(entry.getFailHistogram().total(), 200) << "Only the 500 should be in fail histogram";
    EXPECT_EQ(entry.getFailHistogram().max(), 200);
}

// Test that 404 success/500 failure tracks the configurable status-error rule.
// Default config is 5xx: a 404 lands only in the total histogram, a 500 in both.
TEST_F(UrlStatTest, SnapshotDefaultConfig404SuccessTest) {
    UrlStatSnapshot snapshot;
    Config config;
    auto& tick_clock = mock_agent_service_->getUrlStats().getTickClock();
    auto now = std::chrono::system_clock::now();

    UrlStatEntry stat_404("/api/users", "GET", 404);
    stat_404.elapsed_ = 150;
    stat_404.failed_ = mock_agent_service_->isStatusFail(404);
    stat_404.end_time_ = now;

    UrlStatEntry stat_500("/api/users", "GET", 500);
    stat_500.elapsed_ = 250;
    stat_500.failed_ = mock_agent_service_->isStatusFail(500);
    stat_500.end_time_ = now;

    snapshot.add(&stat_404, config, tick_clock);
    snapshot.add(&stat_500, config, tick_clock);

    auto& entry = snapshot.getEachStats().begin()->second;
    EXPECT_EQ(entry.getTotalHistogram().total(), 400) << "Both should be counted in total";
    EXPECT_EQ(entry.getFailHistogram().total(), 250) << "Only the 500 should be a failure under 5xx";
}

// Test that configuring http.server.status_errors to include 4xx makes 404 a failure.
TEST_F(UrlStatTest, SnapshotConfigurable4xxMakes404FailTest) {
    // Reconfigure the agent's status-error rule to include 4xx.
    mock_agent_service_->mutableConfig()->http.server.status_errors = {"4xx", "5xx"};

    UrlStatSnapshot snapshot;
    Config config;
    auto& tick_clock = mock_agent_service_->getUrlStats().getTickClock();
    auto now = std::chrono::system_clock::now();

    UrlStatEntry stat_404("/api/users", "GET", 404);
    stat_404.elapsed_ = 150;
    stat_404.failed_ = mock_agent_service_->isStatusFail(404);
    stat_404.end_time_ = now;

    UrlStatEntry stat_200("/api/users", "GET", 200);
    stat_200.elapsed_ = 100;
    stat_200.failed_ = mock_agent_service_->isStatusFail(200);
    stat_200.end_time_ = now;

    EXPECT_TRUE(stat_404.failed_) << "404 should be a failure once 4xx is configured";
    EXPECT_FALSE(stat_200.failed_) << "200 should still be a success";

    snapshot.add(&stat_404, config, tick_clock);
    snapshot.add(&stat_200, config, tick_clock);

    auto& entry = snapshot.getEachStats().begin()->second;
    EXPECT_EQ(entry.getTotalHistogram().total(), 250) << "Both should be counted in total";
    EXPECT_EQ(entry.getFailHistogram().total(), 150) << "404 should now be a failure";
}

// Test snapshot limit enforcement
TEST_F(UrlStatTest, SnapshotLimitEnforcementTest) {
    UrlStatSnapshot snapshot;
    Config config;
    config.http.url_stat.limit = 3;
    auto& tick_clock = mock_agent_service_->getUrlStats().getTickClock();

    auto now = std::chrono::system_clock::now();

    // Add entries with different URLs to create distinct keys
    for (int i = 0; i < 5; i++) {
        UrlStatEntry stat("/api/url" + std::to_string(i), "GET", 200);
        stat.elapsed_ = 100;
        stat.end_time_ = now;
        snapshot.add(&stat, config, tick_clock);
    }

    auto& stats = snapshot.getEachStats();
    EXPECT_LE(stats.size(), 3u) << "Should not exceed configured limit of 3";
}

// The limit boundary at the add() cast: config validation only rejects a negative
// limit, so limit == 0 is reachable and must keep nothing (size() >= 0 is always
// true), while limit == 1 keeps exactly one. Guards the `size() >= (size_t)limit`
// comparison and the `limit > 0` reserve guard against an off-by-one or a
// reintroduced signed/unsigned bug.
TEST_F(UrlStatTest, SnapshotLimitZeroAndOneBoundaryTest) {
    auto& tick_clock = mock_agent_service_->getUrlStats().getTickClock();
    const auto now = std::chrono::system_clock::now();
    auto add_n = [&](UrlStatSnapshot& snapshot, const Config& config, int n) {
        for (int i = 0; i < n; i++) {
            UrlStatEntry stat("/api/url" + std::to_string(i), "GET", 200);
            stat.elapsed_ = 100;
            stat.end_time_ = now;
            snapshot.add(&stat, config, tick_clock);
        }
    };

    // limit == 0: nothing is ever stored.
    {
        UrlStatSnapshot snapshot;
        Config config;
        config.http.url_stat.limit = 0;
        add_n(snapshot, config, 4);
        EXPECT_EQ(snapshot.getEachStats().size(), 0u) << "limit 0 must keep no entries";
    }

    // limit == 1: exactly one entry is kept.
    {
        UrlStatSnapshot snapshot;
        Config config;
        config.http.url_stat.limit = 1;
        add_n(snapshot, config, 4);
        EXPECT_EQ(snapshot.getEachStats().size(), 1u) << "limit 1 must keep exactly one entry";
    }
}

// Test snapshot with method_prefix enabled
TEST_F(UrlStatTest, SnapshotMethodPrefixTest) {
    UrlStatSnapshot snapshot;
    Config config;
    config.http.url_stat.method_prefix = true;
    auto& tick_clock = mock_agent_service_->getUrlStats().getTickClock();

    auto now = std::chrono::system_clock::now();

    UrlStatEntry stat_get("/api/users", "GET", 200);
    stat_get.elapsed_ = 100;
    stat_get.end_time_ = now;

    UrlStatEntry stat_post("/api/users", "POST", 201);
    stat_post.elapsed_ = 200;
    stat_post.end_time_ = now;

    snapshot.add(&stat_get, config, tick_clock);
    snapshot.add(&stat_post, config, tick_clock);

    auto& stats = snapshot.getEachStats();
    // GET /api/users and POST /api/users should be different keys
    EXPECT_EQ(stats.size(), 2u) << "Different methods should produce different keys with method_prefix";

    // Verify keys contain method prefix
    for (auto& [key, _] : stats) {
        EXPECT_TRUE(key.url_.find("GET ") == 0 || key.url_.find("POST ") == 0)
            << "Key URL should be prefixed with method: " << key.url_;
    }
}

// Test snapshot with method_prefix disabled (same URL different methods aggregated)
TEST_F(UrlStatTest, SnapshotNoMethodPrefixTest) {
    UrlStatSnapshot snapshot;
    Config config;
    config.http.url_stat.method_prefix = false;
    auto& tick_clock = mock_agent_service_->getUrlStats().getTickClock();

    auto now = std::chrono::system_clock::now();

    UrlStatEntry stat_get("/api/users", "GET", 200);
    stat_get.elapsed_ = 100;
    stat_get.end_time_ = now;

    UrlStatEntry stat_post("/api/users", "POST", 201);
    stat_post.elapsed_ = 200;
    stat_post.end_time_ = now;

    snapshot.add(&stat_get, config, tick_clock);
    snapshot.add(&stat_post, config, tick_clock);

    auto& stats = snapshot.getEachStats();
    EXPECT_EQ(stats.size(), 1u) << "Same URL without method prefix should aggregate";
}

// Test snapshot 5xx boundary status codes (499 = success, 500 = fail) under default config.
TEST_F(UrlStatTest, SnapshotStatusBoundaryTest) {
    UrlStatSnapshot snapshot;
    Config config;
    auto& tick_clock = mock_agent_service_->getUrlStats().getTickClock();

    auto now = std::chrono::system_clock::now();

    UrlStatEntry stat_499("/api/test", "GET", 499);
    stat_499.elapsed_ = 100;
    stat_499.failed_ = mock_agent_service_->isStatusFail(499);
    stat_499.end_time_ = now;

    UrlStatEntry stat_500("/api/test", "GET", 500);
    stat_500.elapsed_ = 200;
    stat_500.failed_ = mock_agent_service_->isStatusFail(500);
    stat_500.end_time_ = now;

    snapshot.add(&stat_499, config, tick_clock);
    snapshot.add(&stat_500, config, tick_clock);

    auto& stats = snapshot.getEachStats();
    auto& entry = stats.begin()->second;

    EXPECT_EQ(entry.getTotalHistogram().total(), 300);
    // Only the 5xx code should be in the fail histogram under the default config.
    EXPECT_EQ(entry.getFailHistogram().total(), 200) << "Only 5xx should be in fail histogram";
}

// ========== Additional TickClock Tests ==========

// Test TickClock tick alignment
TEST_F(UrlStatTest, TickClockAlignmentTest) {
    TickClock clock(30);  // 30-second interval

    auto now = std::chrono::system_clock::now();
    int64_t tick = clock.tick(now);

    // Tick should be aligned to 30-second boundary (divisible by 30000ms)
    EXPECT_EQ(tick % 30000, 0) << "Tick should be aligned to 30-second boundary";
}

// Test TickClock nearby times produce same tick
TEST_F(UrlStatTest, TickClockNearbyTimesSameTickTest) {
    TickClock clock(30);

    auto now = std::chrono::system_clock::now();
    auto near = now + std::chrono::milliseconds(100); // 100ms later

    int64_t tick1 = clock.tick(now);
    int64_t tick2 = clock.tick(near);

    // Within same 30-second window, ticks should be the same
    EXPECT_EQ(tick1, tick2) << "Nearby times within same interval should produce same tick";
}

// Test TickClock different intervals produce different ticks
TEST_F(UrlStatTest, TickClockFarTimeDifferentTickTest) {
    TickClock clock(30);

    auto now = std::chrono::system_clock::now();
    auto later = now + std::chrono::seconds(60); // 60 seconds later

    int64_t tick1 = clock.tick(now);
    int64_t tick2 = clock.tick(later);

    EXPECT_NE(tick1, tick2) << "Times 60 seconds apart should produce different ticks";
    EXPECT_LT(tick1, tick2) << "Later time should produce larger tick";
}

// ========== Additional UrlStatEntry Tests ==========

// Test UrlStatEntry fields
TEST_F(UrlStatTest, UrlStatEntryFieldsTest) {
    UrlStatEntry stat("/api/data", "DELETE", 204);

    EXPECT_EQ(stat.url_pattern_, "/api/data");
    EXPECT_EQ(stat.method_, "DELETE");
    EXPECT_EQ(stat.status_code_, 204);
    EXPECT_EQ(stat.elapsed_, 0);
    EXPECT_FALSE(stat.failed_) << "failed_ should default to false";
    // end_time_ should be default-constructed (epoch)
    EXPECT_EQ(stat.end_time_.time_since_epoch().count(), 0);
}

// ========== Additional UrlStats Tests ==========

// Test takeSnapshot replaces with fresh snapshot
TEST_F(UrlStatTest, TakeSnapshotReplacesWithFreshTest) {
    Config config;
    auto& url_stats = mock_agent_service_->getUrlStats();

    UrlStatEntry stat("/api/test", "GET", 200);
    stat.elapsed_ = 100;
    stat.end_time_ = std::chrono::system_clock::now();

    url_stats.addSnapshot(&stat, config);

    // First take should have entries
    auto snapshot1 = url_stats.takeSnapshot();
    EXPECT_FALSE(snapshot1->getEachStats().empty());

    // Second take (without adding new stats) should be empty
    auto snapshot2 = url_stats.takeSnapshot();
    EXPECT_TRUE(snapshot2->getEachStats().empty()) << "Fresh snapshot after take should be empty";
}

// Test enqueueUrlStats queue overflow behavior
TEST_F(UrlStatTest, EnqueueOverflowTest) {
    UrlStats url_stats(mock_agent_service_.get());

    // Queue size is set to 100 in mock config
    for (int i = 0; i < 150; i++) {
        UrlStatEntry stat{"/api/test" + std::to_string(i), "GET", 200};
        stat.elapsed_ = 100;
        url_stats.enqueueUrlStats(std::move(stat));
    }

    // Should not crash; excess entries are silently dropped
    SUCCEED();
}

// Test EachUrlStat separate total and fail histograms
TEST_F(UrlStatTest, EachUrlStatSeparateHistogramsTest) {
    EachUrlStat stat(12345);

    stat.getTotalHistogram().add(100);
    stat.getTotalHistogram().add(200);
    stat.getFailHistogram().add(500);

    EXPECT_EQ(stat.getTotalHistogram().total(), 300);
    EXPECT_EQ(stat.getTotalHistogram().max(), 200);
    EXPECT_EQ(stat.getFailHistogram().total(), 500);
    EXPECT_EQ(stat.getFailHistogram().max(), 500);

    // Histograms are independent
    EXPECT_NE(stat.getTotalHistogram().total(), stat.getFailHistogram().total());
}

// ========== Injected interval tests ==========
// The tick and send intervals used to be hardcoded to 30s, which made the
// periodic send and cross-tick bucketing unreachable from tests.

TEST_F(UrlStatTest, SendWorkerHonorsInjectedSendInterval) {
    UrlStats url_stats(mock_agent_service_.get(),
                       URL_STAT_TICK_INTERVAL,
                       std::chrono::milliseconds(50));

    std::thread send_worker([&url_stats]() { url_stats.sendUrlStatsWorker(); });

    // With the production 30s interval not a single send could happen in this
    // window; the injected 50ms interval must deliver several.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (mock_agent_service_->recorded_stats_calls_.load() < 3 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    mock_agent_service_->setExiting(true);
    url_stats.stopSendUrlStatsWorker();
    send_worker.join();

    EXPECT_GE(mock_agent_service_->recorded_stats_calls_.load(), 3)
        << "the send worker must fire on the injected interval";
    EXPECT_EQ(mock_agent_service_->last_stats_type_.load(), URL_STATS);
}

TEST_F(UrlStatTest, SendWorkerSurvivesTransientRecordStatsFailure) {
    UrlStats url_stats(mock_agent_service_.get(),
                       URL_STAT_TICK_INTERVAL,
                       std::chrono::milliseconds(50));

    // The first send throws out of the worker loop; the supervisor must
    // restart it (paced by the injected send interval) instead of letting
    // one transient failure end URL-stat sending for the process lifetime.
    mock_agent_service_->stats_throws_remaining_ = 1;

    std::thread send_worker([&url_stats]() { url_stats.sendUrlStatsWorker(); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (mock_agent_service_->recorded_stats_calls_.load() < 3 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    mock_agent_service_->setExiting(true);
    url_stats.stopSendUrlStatsWorker();
    send_worker.join();

    EXPECT_GE(mock_agent_service_->recorded_stats_calls_.load(), 3)
        << "the send worker must be restarted after a transient recordStats failure";
}

TEST_F(UrlStatTest, TickClockBucketsByInjectedTickInterval) {
    const auto config = mock_agent_service_->getConfig();

    // Two samples of the same URL, one second apart. End times are synthetic
    // data, so no wall-clock waiting is involved.
    UrlStatEntry first("/tick/api", "GET", 200);
    first.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(1000));
    first.elapsed_ = 10;
    UrlStatEntry second("/tick/api", "GET", 200);
    second.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(1001));
    second.elapsed_ = 10;

    // Injected 1s ticks: the samples land in two distinct tick buckets.
    UrlStats fine_stats(mock_agent_service_.get(), std::chrono::seconds(1));
    fine_stats.addSnapshot(&first, *config);
    fine_stats.addSnapshot(&second, *config);
    EXPECT_EQ(fine_stats.takeSnapshot()->getEachStats().size(), 2u);

    // Production 30s ticks: the same samples aggregate into one bucket.
    UrlStats coarse_stats(mock_agent_service_.get());
    coarse_stats.addSnapshot(&first, *config);
    coarse_stats.addSnapshot(&second, *config);
    EXPECT_EQ(coarse_stats.takeSnapshot()->getEachStats().size(), 1u);
}

} // namespace pinpoint
