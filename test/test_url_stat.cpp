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
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <thread>
#include <chrono>
#include <type_traits>

#include "../src/url_stat.h"
#include "../src/config.h"
#include "../src/agent_service.h"
#include "../src/grpc_builders.h"
#include "../src/utility.h"
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

TEST_F(UrlStatTest, EachUrlStatDefaultsAreEmptyTest) {
    EachUrlStat stat;

    EXPECT_EQ(stat.total.total(), 0) << "Initial total histogram should be empty";
    EXPECT_EQ(stat.total.max(), 0);
    EXPECT_EQ(stat.fail.total(), 0) << "Initial fail histogram should be empty";
    EXPECT_EQ(stat.fail.max(), 0);
}

TEST_F(UrlStatTest, EachUrlStatHistogramModificationTest) {
    EachUrlStat stat;

    auto& total_hist = stat.total;
    auto& fail_hist = stat.fail;
    
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
    TickClock tick_clock(URL_STAT_TICK_INTERVAL.count());
    snapshot.add(&stat, config, tick_clock);
    
    auto& stats = snapshot.getEachStats();
    EXPECT_FALSE(stats.empty()) << "Snapshot should contain entries after add";
}

TEST_F(UrlStatTest, BuilderReplacesInvalidUtf8InUri) {
    UrlStatSnapshot snapshot;
    Config config;
    config.http.url_stat.enable_trim_path = false;
    config.http.url_stat.method_prefix = false;
    TickClock tick_clock(1);

    UrlStatEntry stat("/caf\xe9/\xff", "GET", 200);
    stat.elapsed_ = 10;
    stat.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(1234));
    snapshot.add(&stat, config, tick_clock);

    google::protobuf::Arena arena;
    const auto* wire = build_url_stat(&snapshot, &arena);
    ASSERT_NE(wire, nullptr);
    ASSERT_EQ(wire->eachuristat_size(), 1);
    EXPECT_EQ(wire->eachuristat(0).uri(), "/caf\xef\xbf\xbd/\xef\xbf\xbd");
    EXPECT_TRUE(isValidUtf8(wire->eachuristat(0).uri()));
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
    EXPECT_EQ(found->first.tick_, 100000);
    EXPECT_EQ(found->second.total.total(), 25);
}

// Callers that record a URI template must turn trimming off, otherwise the
// template they already normalized is collapsed a second time.
TEST_F(UrlStatTest, SnapshotKeepsUriTemplateWhenTrimmingOff) {
    UrlStatSnapshot snapshot;
    Config config;
    config.http.url_stat.enable_trim_path = false;
    TickClock tick_clock(1);

    UrlStatEntry stat("/api/users/{id}", "GET", 200);
    stat.elapsed_ = 30;
    stat.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(200));
    snapshot.add(&stat, config, tick_clock);

    const auto& stats = snapshot.getEachStats();
    ASSERT_EQ(stats.size(), 1u);
    EXPECT_EQ(stats.begin()->first.url_, "/api/users/{id}")
        << "the recorded URI template must be aggregated verbatim by default";
}

// Trimming folds a raw URL's path parameters into one key instead of one key
// per id. Depth 1 is the pre-default-change behaviour, kept reachable by
// configuration for anyone whose dashboards were built on the collapsed keys.
TEST_F(UrlStatTest, SnapshotTrimsRawUrlPathAtDepthOne) {
    UrlStatSnapshot snapshot;
    Config config;
    config.http.url_stat.enable_trim_path = true;
    config.http.url_stat.trim_path_depth = 1;
    TickClock tick_clock(1);

    UrlStatEntry stat("/api/users/123", "GET", 200);
    stat.elapsed_ = 30;
    stat.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(200));
    snapshot.add(&stat, config, tick_clock);

    const auto& stats = snapshot.getEachStats();
    ASSERT_EQ(stats.size(), 1u);
    EXPECT_EQ(stats.begin()->first.url_, "/api/*");
}

// The shipped default (trim on, depth 3): a route-shaped path survives whole,
// so C++ keys equal what Java and Go would have recorded verbatim. Uses a
// default-constructed Config on purpose — this pins the default itself.
TEST_F(UrlStatTest, SnapshotKeepsRouteShapedPathAtDefaultDepth) {
    Config config;
    ASSERT_TRUE(config.http.url_stat.enable_trim_path);
    ASSERT_EQ(config.http.url_stat.trim_path_depth, 3);
    TickClock tick_clock(1);

    for (const auto* url : {"/api/users/123", "/api/users/{id}"}) {
        UrlStatSnapshot snapshot;
        UrlStatEntry stat(url, "GET", 200);
        stat.elapsed_ = 30;
        stat.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(200));
        snapshot.add(&stat, config, tick_clock);

        const auto& stats = snapshot.getEachStats();
        ASSERT_EQ(stats.size(), 1u);
        EXPECT_EQ(stats.begin()->first.url_, url)
            << "the default depth must not collapse a route-shaped path";
    }

    // Deeper than the default depth still folds, which is the whole point of
    // leaving trimming on for raw-URL callers.
    UrlStatSnapshot deep;
    UrlStatEntry stat("/api/users/123/comments/9", "GET", 200);
    stat.elapsed_ = 30;
    stat.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(200));
    deep.add(&stat, config, tick_clock);
    ASSERT_EQ(deep.getEachStats().size(), 1u);
    EXPECT_EQ(deep.getEachStats().begin()->first.url_, "/api/users/123/*");
}

TEST_F(UrlStatTest, SnapshotBucketsEmptyUrlUnderUnknownConstant) {
    UrlStatSnapshot snapshot;
    Config config;
    TickClock tick_clock(1);

    UrlStatEntry stat("", "GET", 200);
    stat.elapsed_ = 5;
    stat.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(300));
    snapshot.add(&stat, config, tick_clock);

    UrlStatSnapshot trimmed_snapshot;
    Config trim_config;
    trim_config.http.url_stat.enable_trim_path = true;
    trimmed_snapshot.add(&stat, trim_config, tick_clock);

    // The literal, not just the constant: the value is Java's
    // URITemplate.NULL_URI, so a rename would split the mixed-language
    // "no URI recorded" bucket in two without any test noticing.
    EXPECT_EQ(URL_STAT_UNKNOWN, "/NULL");
    ASSERT_EQ(snapshot.getEachStats().size(), 1u);
    EXPECT_EQ(snapshot.getEachStats().begin()->first.url_, URL_STAT_UNKNOWN);
    ASSERT_EQ(trimmed_snapshot.getEachStats().size(), 1u);
    EXPECT_EQ(trimmed_snapshot.getEachStats().begin()->first.url_, URL_STAT_UNKNOWN)
        << "trimming must not turn the stand-in key back into an empty string";
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

// An empty histogram travels as an empty message, not as eight zero buckets.
// The failed histogram is empty for every URI that never failed, so those
// eight zeroes would otherwise ride along with every URI on every tick.
// Matches Java's UriStatMapper.checkEmptyThenMap and Go's makePUriHistogram.
TEST_F(UrlStatTest, EmptyFailedHistogramIsSerializedAsAnEmptyMessage) {
    UrlStatSnapshot snapshot;
    Config config;
    config.http.url_stat.enable_trim_path = false;
    config.http.url_stat.method_prefix = false;
    TickClock tick_clock(1);

    UrlStatEntry stat("/ok", "GET", 200);
    stat.elapsed_ = 10;
    stat.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(1234));
    snapshot.add(&stat, config, tick_clock);

    google::protobuf::Arena arena;
    const auto* wire = build_url_stat(&snapshot, &arena);
    ASSERT_NE(wire, nullptr);
    ASSERT_EQ(wire->eachuristat_size(), 1);

    const auto& each = wire->eachuristat(0);
    // The message must still be present: Java substitutes
    // PUriHistogram.getDefaultInstance(), it does not clear the field.
    ASSERT_TRUE(each.has_failedhistogram());
    EXPECT_EQ(each.failedhistogram().histogram_size(), 0);
    EXPECT_EQ(each.failedhistogram().total(), 0);
    EXPECT_EQ(each.failedhistogram().max(), 0);
    EXPECT_EQ(each.failedhistogram().ByteSizeLong(), 0u)
        << "an empty histogram must not put anything on the wire";

    // The non-empty total histogram is what is left, so the whole record must
    // not carry a second packed run of eight buckets.
    EXPECT_EQ(each.totalhistogram().histogram_size(), URL_STATS_BUCKET_SIZE);
}

// The other half of the rule: a histogram with samples still sends all eight
// buckets, including the zero ones between the populated ones.
TEST_F(UrlStatTest, NonEmptyHistogramStillSendsEveryBucket) {
    UrlStatSnapshot snapshot;
    Config config;
    config.http.url_stat.enable_trim_path = false;
    config.http.url_stat.method_prefix = false;
    TickClock tick_clock(1);

    UrlStatEntry stat("/slow", "GET", 500);
    stat.elapsed_ = 9000;  // last bucket
    stat.failed_ = true;
    stat.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(1234));
    snapshot.add(&stat, config, tick_clock);

    google::protobuf::Arena arena;
    const auto* wire = build_url_stat(&snapshot, &arena);
    ASSERT_NE(wire, nullptr);
    ASSERT_EQ(wire->eachuristat_size(), 1);

    const auto& failed = wire->eachuristat(0).failedhistogram();
    ASSERT_EQ(failed.histogram_size(), URL_STATS_BUCKET_SIZE);
    EXPECT_EQ(failed.histogram(URL_STATS_BUCKET_SIZE - 1), 1);
    EXPECT_EQ(failed.histogram(0), 0);
    EXPECT_EQ(failed.total(), 9000);
    EXPECT_EQ(failed.max(), 9000);
}

// A histogram of nothing but 0ms samples has total() == 0 and max() == 0, so
// only the bucket counts can tell it apart from a histogram of no samples.
TEST_F(UrlStatTest, ZeroElapsedSampleIsNotAnEmptyHistogram) {
    UrlStatHistogram histogram;
    EXPECT_TRUE(histogram.empty());

    histogram.add(0);
    EXPECT_FALSE(histogram.empty()) << "a 0ms sample is still a sample";
    EXPECT_EQ(histogram.total(), 0);
    EXPECT_EQ(histogram.max(), 0);
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
    EXPECT_EQ(first_stat->second.total.total(), 10);
    EXPECT_EQ(second_stat->second.total.total(), 20);
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
    EXPECT_EQ(stats.begin()->second.total.total(), 129);
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
    
    // Take snapshot. The entry's tick is still in progress and nothing has
    // been cut, so only the shutdown flush form returns it.
    auto snapshot = mock_agent_service_->getUrlStats().takeSnapshot(true);
    EXPECT_NE(snapshot.get(), nullptr) << "Snapshot should not be null";
    
    auto& stats = snapshot->getEachStats();
    EXPECT_FALSE(stats.empty()) << "Snapshot should contain the added stat";
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
    
    mock_agent_service_->setExiting(true);
    url_stats.stopAddUrlStatsWorker();
    add_worker.join();
    
    // Take snapshot and verify. All three entries share the tick in
    // progress, so the flush form is what returns them.
    auto snapshot = url_stats.takeSnapshot(true);
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

// ========== trim_url_path Tests ==========

TEST_F(UrlStatTest, TrimUrlPathTest) {
    struct Case {
        std::string path;
        int depth;
        std::string expected;
    };
    const Case cases[] = {
        {"", 3, ""},                        // empty path
        {"/", 3, "/"},                      // root only
        {"/api", 1, "/api"},                // single level path preserved
        {"/api/users", 10, "/api/users"},   // depth exceeds segments: full path
        {"/api/v1/users", 2, "/api/v1/*"},  // trimmed after depth 2
        {"/api/v1/users/123", 1, "/api/*"}, // trimmed after depth 1
        {"/api/v1/users", -5, "/api/*"},    // depth < 1 gets converted to 1
        {"/api?key=value", 3, "/api"},      // query params removed
        // Each '/' decrements depth, so "/api/" uses depth 1, "/" uses depth 2 -> trim
        {"/api//v1/users", 2, "/api//*"},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE("trim_url_path(\"" + c.path + "\", " + std::to_string(c.depth) + ")");
        EXPECT_EQ(UrlStatSnapshot::trim_url_path(c.path, c.depth), c.expected);
    }
}

// ========== Additional UrlStatSnapshot Tests ==========

TEST_F(UrlStatTest, SnapshotAggregatesSameUrlAndTickTest) {
    UrlStatSnapshot snapshot;
    Config config;
    TickClock tick_clock(URL_STAT_TICK_INTERVAL.count());

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
    EXPECT_EQ(entry.total.total(), 300) << "Total should be 100 + 200";
    EXPECT_EQ(entry.total.max(), 200);
}

// Test snapshot fail aggregation honors the precomputed failed_ flag.
// With the default config (5xx) a 404 is a success; only a 500 is a failure.
TEST_F(UrlStatTest, SnapshotFailStatusAggregationTest) {
    UrlStatSnapshot snapshot;
    Config config;
    TickClock tick_clock(URL_STAT_TICK_INTERVAL.count());

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
    EXPECT_EQ(entry.total.total(), 450) << "All 3 should be in total histogram";
    EXPECT_EQ(entry.fail.total(), 200) << "Only the 500 should be in fail histogram";
    EXPECT_EQ(entry.fail.max(), 200);
}

// Test that 404 success/500 failure tracks the configurable status-error rule.
// Default config is 5xx: a 404 lands only in the total histogram, a 500 in both.
TEST_F(UrlStatTest, SnapshotDefaultConfig404SuccessTest) {
    UrlStatSnapshot snapshot;
    Config config;
    TickClock tick_clock(URL_STAT_TICK_INTERVAL.count());
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
    EXPECT_EQ(entry.total.total(), 400) << "Both should be counted in total";
    EXPECT_EQ(entry.fail.total(), 250) << "Only the 500 should be a failure under 5xx";
}

// Test that configuring http.server.status_errors to include 4xx makes 404 a failure.
TEST_F(UrlStatTest, SnapshotConfigurable4xxMakes404FailTest) {
    // Reconfigure the agent's status-error rule to include 4xx.
    mock_agent_service_->mutableConfig()->http.server.status_errors = {"4xx", "5xx"};

    UrlStatSnapshot snapshot;
    Config config;
    TickClock tick_clock(URL_STAT_TICK_INTERVAL.count());
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
    EXPECT_EQ(entry.total.total(), 250) << "Both should be counted in total";
    EXPECT_EQ(entry.fail.total(), 150) << "404 should now be a failure";
}

TEST_F(UrlStatTest, SnapshotLimitEnforcementTest) {
    UrlStatSnapshot snapshot;
    Config config;
    config.http.url_stat.limit = 3;
    TickClock tick_clock(URL_STAT_TICK_INTERVAL.count());

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
    TickClock tick_clock(URL_STAT_TICK_INTERVAL.count());
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
    TickClock tick_clock(URL_STAT_TICK_INTERVAL.count());

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
    TickClock tick_clock(URL_STAT_TICK_INTERVAL.count());

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
    TickClock tick_clock(URL_STAT_TICK_INTERVAL.count());

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

    EXPECT_EQ(entry.total.total(), 300);
    // Only the 5xx code should be in the fail histogram under the default config.
    EXPECT_EQ(entry.fail.total(), 200) << "Only 5xx should be in fail histogram";
}

// ========== Additional TickClock Tests ==========

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

TEST_F(UrlStatTest, TakeSnapshotReplacesWithFreshTest) {
    Config config;
    auto& url_stats = mock_agent_service_->getUrlStats();

    UrlStatEntry stat("/api/test", "GET", 200);
    stat.elapsed_ = 100;
    stat.end_time_ = std::chrono::system_clock::now();

    url_stats.addSnapshot(&stat, config);

    // First take should have entries
    auto snapshot1 = url_stats.takeSnapshot(true);
    EXPECT_FALSE(snapshot1->getEachStats().empty());

    // Second take (without adding new stats) should be empty
    auto snapshot2 = url_stats.takeSnapshot(true);
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
    EachUrlStat stat;

    stat.total.add(100);
    stat.total.add(200);
    stat.fail.add(500);

    EXPECT_EQ(stat.total.total(), 300);
    EXPECT_EQ(stat.total.max(), 200);
    EXPECT_EQ(stat.fail.total(), 500);
    EXPECT_EQ(stat.fail.max(), 500);

    // Histograms are independent
    EXPECT_NE(stat.total.total(), stat.fail.total());
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
    EXPECT_EQ(fine_stats.takeSnapshot(true)->getEachStats().size(), 2u);

    // Production 30s ticks: the same samples aggregate into one bucket.
    UrlStats coarse_stats(mock_agent_service_.get());
    coarse_stats.addSnapshot(&first, *config);
    coarse_stats.addSnapshot(&second, *config);
    EXPECT_EQ(coarse_stats.takeSnapshot(true)->getEachStats().size(), 1u);
}

// ========== Tick-boundary snapshots, limit drops, method prefix ==========

// Captures what the URL-stat paths log. The only observable effect of a
// limit/retention drop is its WARN, so these tests read the log file back.
class UrlStatLogTest : public UrlStatTest {
protected:
    void SetUp() override {
        UrlStatTest::SetUp();
        log_file_ = std::filesystem::temp_directory_path() / "test_pinpoint_url_stat_log.txt";
        std::filesystem::remove(log_file_);
        Logger::getInstance().setLogLevel("warning");
        Logger::getInstance().setFileLogger(log_file_.string(), 10);
    }

    void TearDown() override {
        Logger::getInstance().shutdown();
        std::filesystem::remove(log_file_);
        UrlStatTest::TearDown();
    }

    // Flushes the logger, then returns everything written so far.
    std::string logged() {
        Logger::getInstance().shutdown();
        std::ifstream file(log_file_);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    std::filesystem::path log_file_;
};

// Feeds one entry for `url` in the tick starting at `second`.
static void add_at(UrlStats& stats, const Config& config, const std::string& url, int64_t second) {
    UrlStatEntry entry(url, "GET", 200);
    entry.elapsed_ = 10;
    entry.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(second));
    stats.addSnapshot(&entry, config);
}

// The regression this whole change exists for: while the stats stream is
// stalled, `limit` must stay a per-tick capacity. Before tick-boundary
// snapshots, one map accumulated every stalled tick's keys and `limit`
// rejected everything past the first `limit` keys of the whole outage — a
// 5-minute stall cut each tick's capacity to a tenth.
TEST_F(UrlStatTest, LimitStaysPerTickWhileSendingIsBlocked) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->http.url_stat.enable_trim_path = false;
    cfg->http.url_stat.limit = 3;
    const auto config = mock_agent_service_->getConfig();

    // 1s ticks stand in for the production 30s ones; ten ticks are the
    // 5-minute stall. No takeSnapshot() in the loop: nothing is sent.
    UrlStats url_stats(mock_agent_service_.get(), std::chrono::seconds(1));
    constexpr int kTicks = 10;
    for (int t = 0; t < kTicks; t++) {
        for (int u = 0; u < 3; u++) {
            add_at(url_stats, *config, "/api/tick" + std::to_string(t) + "/url" + std::to_string(u),
                   1000 + t);
        }
    }

    const auto snapshot = url_stats.takeSnapshot(true);
    std::unordered_map<int64_t, int> per_tick;
    for (const auto& [key, unused] : snapshot->getEachStats()) {
        per_tick[key.tick_]++;
    }

    // Four completed ticks are retained (Java's snapshotQueue capacity) plus
    // the one still in progress; the older six were dropped whole.
    EXPECT_EQ(per_tick.size(), 5u) << "retention is bounded at 4 completed ticks + the current one";
    for (const auto& [tick, count] : per_tick) {
        EXPECT_EQ(count, 3) << "tick " << tick << " must get the full limit, not a share of it";
    }
    EXPECT_EQ(snapshot->getEachStats().size(), 15u)
        << "a stalled stream must not shrink total capacity to a single limit";
}

// Stragglers: an entry for an already-cut tick must be kept (under its own
// tick key) and must not cut the snapshot again.
TEST_F(UrlStatTest, StragglerForCutTickIsFoldedBackOnSend) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->http.url_stat.enable_trim_path = false;
    const auto config = mock_agent_service_->getConfig();

    UrlStats url_stats(mock_agent_service_.get(), std::chrono::seconds(1));
    add_at(url_stats, *config, "/api/straggler", 1000);  // opens tick 1000
    add_at(url_stats, *config, "/api/next", 1001);       // cuts tick 1000
    add_at(url_stats, *config, "/api/straggler", 1000);  // late arrival for tick 1000

    const auto snapshot = url_stats.takeSnapshot(true);
    const auto& stats = snapshot->getEachStats();
    ASSERT_EQ(stats.size(), 2u) << "the straggler must not open a third key";
    const auto found = stats.find(UrlKey{"/api/straggler", 1000000});
    ASSERT_NE(found, stats.end());
    EXPECT_EQ(found->second.total.total(), 20)
        << "both samples of the straggling key must survive the merge";
}

TEST_F(UrlStatLogTest, LimitOverflowWarnsAndRateLimits) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->http.url_stat.enable_trim_path = false;
    cfg->http.url_stat.limit = 1;
    const auto config = mock_agent_service_->getConfig();

    UrlStats url_stats(mock_agent_service_.get(), std::chrono::seconds(1));
    add_at(url_stats, *config, "/api/kept", 1000);
    for (int i = 0; i < 50; i++) {
        add_at(url_stats, *config, "/api/dropped" + std::to_string(i), 1000);
    }

    const auto content = logged();
    EXPECT_NE(content.find("url stat limit reached"), std::string::npos)
        << "an over-limit drop must not be silent";
    EXPECT_NE(content.find("max 1 distinct urls per tick"), std::string::npos)
        << "the log must name the limit that rejected the url";
    // The default reporter interval is 60s, so all 50 drops fold into one line.
    EXPECT_EQ(std::count(content.begin(), content.end(), '\n'), 1)
        << "50 drops must not produce 50 log lines";
}

TEST_F(UrlStatLogTest, CompletedSnapshotQueueDropsOldestAndReports) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->http.url_stat.enable_trim_path = false;
    const auto config = mock_agent_service_->getConfig();

    // Seven ticks: six of them get cut into a queue with four slots, so the
    // two oldest are evicted. The seventh is still in progress.
    UrlStats url_stats(mock_agent_service_.get(), std::chrono::seconds(1));
    for (int t = 0; t < 7; t++) {
        add_at(url_stats, *config, "/api/tick" + std::to_string(t), 1000 + t);
    }

    const auto snapshot = url_stats.takeSnapshot(true);
    const auto& stats = snapshot->getEachStats();
    EXPECT_EQ(stats.size(), 5u) << "4 retained ticks + the one in progress";
    EXPECT_EQ(stats.find(UrlKey{"/api/tick0", 1000000}), stats.end()) << "the oldest tick goes first";
    EXPECT_EQ(stats.find(UrlKey{"/api/tick1", 1001000}), stats.end()) << "then the next-oldest";
    EXPECT_NE(stats.find(UrlKey{"/api/tick2", 1002000}), stats.end()) << "newer ticks are kept";
    EXPECT_NE(stats.find(UrlKey{"/api/tick6", 1006000}), stats.end()) << "including the one in progress";

    const auto content = logged();
    EXPECT_NE(content.find("url stat snapshot queue overflow"), std::string::npos)
        << "discarding a whole tick must not be silent";
}

// A prefix built from an empty method used to produce " /api/users": the same
// url split into two server-side keys depending on whether the method was
// known. Java's UriMethodTransformer and Go both skip the prefix instead.
TEST_F(UrlStatTest, MethodPrefixIsSkippedForEmptyMethod) {
    UrlStatSnapshot snapshot;
    Config config;
    config.http.url_stat.enable_trim_path = false;
    config.http.url_stat.method_prefix = true;
    TickClock tick_clock(1);

    UrlStatEntry no_method("/api/users", "", 200);
    no_method.elapsed_ = 10;
    no_method.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(400));
    snapshot.add(&no_method, config, tick_clock);

    const auto& stats = snapshot.getEachStats();
    ASSERT_EQ(stats.size(), 1u);
    EXPECT_EQ(stats.begin()->first.url_, "/api/users") << "an empty method must not leave a prefix";

    // A known method still gets one, so the fix does not disable the feature.
    UrlStatEntry with_method("/api/users", "GET", 200);
    with_method.elapsed_ = 10;
    with_method.end_time_ = no_method.end_time_;
    snapshot.add(&with_method, config, tick_clock);
    EXPECT_NE(stats.find(UrlKey{"GET /api/users", 400000}), stats.end());
}


// ========== Completed-tick-only sends ==========

// The split-send regression. tick and send intervals are both 30s but free
// running, so a send lands mid-tick roughly always. Taking the tick in
// progress put half of it in one message and half in the next: the server
// sums the counts back up, but the per-tick max and the total/count average
// are computed per message, so one tick reported two maxima and two averages.
TEST_F(UrlStatTest, SendsInsideOneTickDoNotSplitIt) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->http.url_stat.enable_trim_path = false;
    const auto config = mock_agent_service_->getConfig();

    UrlStats url_stats(mock_agent_service_.get(), std::chrono::seconds(30));
    const UrlKey split_key{"/api/split", 0};

    // Two samples inside tick 0, with a send between them.
    add_at(url_stats, *config, "/api/split", 1);
    const auto first = url_stats.takeSnapshot();
    add_at(url_stats, *config, "/api/split", 20);
    // The first entry of tick 30000 closes tick 0.
    add_at(url_stats, *config, "/api/other", 40);
    const auto second = url_stats.takeSnapshot();

    int messages_carrying_the_key = 0;
    for (const auto* snapshot : {first.get(), second.get()}) {
        if (snapshot->getEachStats().count(split_key) != 0) {
            messages_carrying_the_key++;
        }
    }
    EXPECT_EQ(messages_carrying_the_key, 1)
        << "one (uri, tick) key must never be spread across two messages";

    EXPECT_TRUE(first->empty()) << "a send before the tick closed has nothing completed to carry";
    const auto found = second->getEachStats().find(split_key);
    ASSERT_NE(found, second->getEachStats().end());
    EXPECT_EQ(found->second.total.total(), 20) << "both samples must arrive in the same message";
    EXPECT_EQ(found->second.total.histogram(0), 2);
}

// An idle agent used to send an empty PAgentUriStat once per send interval.
TEST_F(UrlStatTest, NoTrafficLeavesNothingToSend) {
    UrlStats url_stats(mock_agent_service_.get(), std::chrono::seconds(1));

    for (int send = 0; send < 3; send++) {
        EXPECT_TRUE(url_stats.takeSnapshot()->empty())
            << "send " << send << " has no completed tick, so there is no message to build";
    }
}

// The boundary case: once a tick is cut, the very next send carries all of it
// and nothing of the tick that replaced it.
TEST_F(UrlStatTest, CutTickGoesOutWholeInTheNextMessage) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->http.url_stat.enable_trim_path = false;
    const auto config = mock_agent_service_->getConfig();

    UrlStats url_stats(mock_agent_service_.get(), std::chrono::seconds(1));
    add_at(url_stats, *config, "/api/a", 1000);
    add_at(url_stats, *config, "/api/b", 1000);
    add_at(url_stats, *config, "/api/a", 1000);
    add_at(url_stats, *config, "/api/a", 1001);  // cuts tick 1000

    const auto snapshot = url_stats.takeSnapshot();
    const auto& stats = snapshot->getEachStats();
    ASSERT_EQ(stats.size(), 2u) << "only tick 1000's two keys";
    for (const auto& [key, unused] : stats) {
        EXPECT_EQ(key.tick_, 1000000) << "the tick still in progress must stay behind";
    }
    const auto found = stats.find(UrlKey{"/api/a", 1000000});
    ASSERT_NE(found, stats.end());
    EXPECT_EQ(found->second.total.total(), 20) << "every sample of the cut tick, and only those";
}

// Nothing arrives after shutdown to cut the tick in progress, so there it is
// split-or-lose: the flush form takes it, once, on the way out.
TEST_F(UrlStatTest, ShutdownFlushTakesTheTickStillInProgress) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->http.url_stat.enable_trim_path = false;
    const auto config = mock_agent_service_->getConfig();

    UrlStats url_stats(mock_agent_service_.get(), std::chrono::seconds(1));
    add_at(url_stats, *config, "/api/done", 1000);
    add_at(url_stats, *config, "/api/live", 1001);  // cuts tick 1000, opens 1001

    const auto regular = url_stats.takeSnapshot();
    ASSERT_EQ(regular->getEachStats().size(), 1u);
    EXPECT_NE(regular->getEachStats().find(UrlKey{"/api/done", 1000000}),
              regular->getEachStats().end());

    const auto flushed = url_stats.takeSnapshot(true);
    ASSERT_EQ(flushed->getEachStats().size(), 1u) << "the trailing tick must go out last, not be stranded";
    EXPECT_NE(flushed->getEachStats().find(UrlKey{"/api/live", 1001000}),
              flushed->getEachStats().end());

    EXPECT_TRUE(url_stats.takeSnapshot(true)->empty()) << "the flush must leave nothing behind";
}


// A tick with no successor traffic is still over once its window elapses.
// Cutting only on the arrival of a newer entry (addLocked) covers an agent
// under load; an agent that goes quiet would hold its last tick until traffic
// resumed. Java closes it on the clock instead
// (AsyncQueueingUriStatStorage.checkAndFlushOldData).
TEST_F(UrlStatTest, ElapsedTickIsClosedWithoutNewerTraffic) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->http.url_stat.enable_trim_path = false;
    const auto config = mock_agent_service_->getConfig();

    // Real wall-clock end time: closeElapsedTick asks the clock, not the
    // entries, so this is the one test that cannot use synthetic ticks.
    UrlStats url_stats(mock_agent_service_.get(), std::chrono::seconds(1));
    UrlStatEntry entry("/api/quiet", "GET", 200);
    entry.elapsed_ = 10;
    entry.end_time_ = std::chrono::system_clock::now();
    url_stats.addSnapshot(&entry, *config);

    url_stats.closeElapsedTick();
    EXPECT_TRUE(url_stats.takeSnapshot()->empty())
        << "the tick is still open, so nothing may be sent yet";

    // 1.1x the tick width, so the boundary is crossed wherever inside the
    // tick the entry landed.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    url_stats.closeElapsedTick();

    const auto snapshot = url_stats.takeSnapshot();
    ASSERT_EQ(snapshot->getEachStats().size(), 1u)
        << "an elapsed tick must be sent even though no newer entry ever arrived";
    EXPECT_EQ(snapshot->getEachStats().begin()->second.total.total(), 10);

    // Idempotent: nothing left to close, and no empty tick invented.
    url_stats.closeElapsedTick();
    EXPECT_TRUE(url_stats.takeSnapshot()->empty());
}

// The clock close must not manufacture ticks out of an idle agent.
TEST_F(UrlStatTest, CloseElapsedTickOnAnIdleAgentProducesNothing) {
    UrlStats url_stats(mock_agent_service_.get(), std::chrono::seconds(1));
    for (int i = 0; i < 3; i++) {
        url_stats.closeElapsedTick();
        EXPECT_TRUE(url_stats.takeSnapshot()->empty()) << "cycle " << i;
    }
}

} // namespace pinpoint
