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


#include "pinpoint/tracer.h"

#include "../src/active_span.h"
#include "../src/agent_service.h"
#include "../src/annotation.h"
#include "../src/cache.h"
#include "../src/config.h"
#include "../src/grpc_builders.h"
#include "../src/http.h"
#include "../src/limiter.h"
#include "../src/logging.h"
#include "../src/sampling.h"
#include "../src/sharded_bounded_queue.h"
#include "../src/span.h"
#include "../src/sql.h"
#include "../src/grpc.h"
#include "../src/url_stat.h"
#include "../src/utility.h"
#include "v1/Span.pb.h"

#include <gtest/gtest.h>

#include <google/protobuf/arena.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pinpoint {


namespace {

// splitOutputParams is the agent-side counterpart of the server's
// OutputParameterParser: it splits `parameters` on ',' and un-escapes the
// doubled ',,' that a literal containing a comma produces. Kept next to the
// invariant below; test_sql.cpp has its own copy for the round-trip cases.
std::vector<std::string> splitOutputParams(std::string_view params) {
    std::vector<std::string> out;
    if (params.empty()) {
        return out;
    }
    std::string cur;
    for (size_t i = 0; i < params.size(); ++i) {
        if (params[i] != ',') {
            cur.push_back(params[i]);
            continue;
        }
        if (i + 1 < params.size() && params[i + 1] == ',') {
            cur.push_back(',');
            ++i;
            continue;
        }
        out.push_back(cur);
        cur.clear();
    }
    out.push_back(cur);
    return out;
}

// scanPlaceholderIndices returns the placeholder indices of a normalized
// statement in the order they appear. `<n>#` marks a number and `<n>$` a
// character literal; both draw from one shared counter, which is what lets the
// server refill them from a single comma-separated parameter string.
std::vector<int> scanPlaceholderIndices(std::string_view normalized) {
    std::vector<int> out;
    std::string digits;
    for (const char ch : normalized) {
        if (ch >= '0' && ch <= '9') {
            digits.push_back(ch);
            continue;
        }
        if ((ch == '#' || ch == '$') && !digits.empty()) {
            out.push_back(std::stoi(digits));
        }
        digits.clear();
    }
    return out;
}

}  // namespace

// The placeholder indices must run 0..n-1 in order, there must be exactly as
// many of them as there are parameters, and param_index must agree. This is
// the property that breaks first if numbers and literals ever start counting
// separately, and it breaks silently: every statement still normalizes, the
// server just refills the wrong values.
TEST(JavaParityLockTest, SqlNormalizerSharedIndexCounter) {
    const SqlNormalizer normalizer(kMaxNormalizedSqlLength, false);

    const char* statements[] = {
        R"(select * from t where a = 1.5e3 and b = 'it''s' and c = "col1" -- comment)",
        R"(a = -1 and b = 1e-3 and c = 0x1F)",
        R"(select t1.col2, t1.5 from t where id = 10 and name = 'a,b' and n2 = 'x''y')",
        R"(select /*+ INDEX(t idx) */ 1, "2", '' , 'z' from t /* multi
 line 42 */ where x = ?)",
        R"(SELECT/*c*/1 FROM t WHERE table1 = 2 and name2 = 'v')",
        R"(select $1, $2 from t where a=$3 and b = 4)",
        R"(insert into t values (1,2,3), ('a','b','c') // trailing)",
        R"(select '''' from t where a = 1)",
        R"($'x'1)",
        R"(1.4e-10)",
        R"(select #1 from t)",
        R"(select * from t where a in (1,2,3))",
        R"(select * from t where a in (?, ?, ?))",
        R"(/*/)",
        R"(V$SESSION1)",
        R"(test.123)",
        R"(test_ 123)",
    };

    for (const char* sql : statements) {
        const auto result = normalizer.normalize(sql);
        const auto indices = scanPlaceholderIndices(result.normalized_sql);
        const auto params = splitOutputParams(result.parameters);

        for (size_t i = 0; i < indices.size(); ++i) {
            EXPECT_EQ(indices[i], static_cast<int>(i))
                << "placeholder indices must run 0..n-1 in order: " << sql;
        }
        EXPECT_EQ(params.size(), indices.size())
            << "one parameter per placeholder: " << sql;
        EXPECT_EQ(result.param_index, static_cast<int>(indices.size()))
            << "param_index is the number of placeholders emitted: " << sql;
    }
}

TEST(JavaParityLockTest, SqlNormalizerIsNotIdempotent) {
    const SqlNormalizer normalizer(kMaxNormalizedSqlLength, false);

    const auto once = normalizer.normalize("select 1");
    EXPECT_EQ(once.normalized_sql, "select 0#");

    const auto twice = normalizer.normalize(once.normalized_sql);
    EXPECT_EQ(twice.normalized_sql, "select 0##")
        << "normalization is not idempotent in any of the three agents";
}

// Runs of whitespace survive verbatim - none of the three agents collapse
// them, so two statements differing only in spacing are two SQL ids.
TEST(JavaParityLockTest, SqlNormalizerWhitespaceIsNotNormalized) {
    const SqlNormalizer normalizer(kMaxNormalizedSqlLength, false);
    const auto result = normalizer.normalize("select   *\n\tfrom  t");
    EXPECT_EQ(result.normalized_sql, "select   *\n\tfrom  t");
}

// An empty literal consumes no index in any of the three agents.
TEST(JavaParityLockTest, SqlNormalizerEmptyLiteralConsumesNoIndex) {
    const SqlNormalizer normalizer(kMaxNormalizedSqlLength, false);

    const auto result = normalizer.normalize("select '', 1 from t");
    EXPECT_EQ(result.normalized_sql, "select '', 0# from t");
    EXPECT_EQ(result.parameters, "1");
    EXPECT_EQ(result.param_index, 1);
}

TEST(JavaParityLockTest, SqlNormalizerOversizeStatementIsDroppedWhole) {
    EXPECT_EQ(kMaxNormalizedSqlLength, 1024u * 1024u)
        << "Go maxSqlNormalizeLength = 1 << 20";

    const SqlNormalizer normalizer(kMaxNormalizedSqlLength, false);

    std::string at_cap = "select ";
    at_cap.append(kMaxNormalizedSqlLength - at_cap.size(), 'a');
    ASSERT_EQ(at_cap.size(), kMaxNormalizedSqlLength);
    // EXPECT_TRUE, not EXPECT_EQ: a failure here must not print a megabyte.
    EXPECT_TRUE(normalizer.normalize(at_cap).normalized_sql == at_cap)
        << "exactly at the cap is still normalized, whole";

    const std::string over_cap = at_cap + "a";
    const auto dropped = normalizer.normalize(over_cap);
    EXPECT_TRUE(dropped.normalized_sql.empty()) << "one byte over the cap drops the statement";
    EXPECT_TRUE(dropped.parameters.empty()) << "and its parameters with it";
    EXPECT_EQ(dropped.param_index, 0);
}

// ===========================================================================
// Group 2 - span event depth / sequence numbering
// ===========================================================================

TEST(JavaParityLockTest, SpanEventLimitDefaults) {
    EXPECT_EQ(defaults::SPAN_MAX_EVENT_DEPTH, 64) << "Java profiler.callstack.max.depth";
    EXPECT_EQ(defaults::SPAN_MAX_EVENT_SEQUENCE, 5000) << "Java profiler.callstack.max.sequence";
    EXPECT_EQ(defaults::SPAN_EVENT_CHUNK_SIZE, 20) << "Java profiler.io.buffering.buffersize";
}

namespace {

bool javaOverflowDecision(int32_t sequence, int32_t depth, int32_t max_sequence, int32_t max_depth) {
    return sequence >= max_sequence || depth - 1 > max_depth;
}

}  // namespace

// The effective deepest recorded level is maxDepth + 1, and exactly
// maxSequence events are recorded.
TEST(JavaParityLockTest, SpanEventOverflowBoundaries) {
    constexpr int32_t kMaxDepth = 3;
    constexpr int32_t kMaxSequence = 5;

    EXPECT_FALSE(javaOverflowDecision(0, kMaxDepth, kMaxSequence, kMaxDepth));
    EXPECT_FALSE(javaOverflowDecision(0, kMaxDepth + 1, kMaxSequence, kMaxDepth))
        << "the deepest recorded level is maxDepth + 1";
    EXPECT_TRUE(javaOverflowDecision(0, kMaxDepth + 2, kMaxSequence, kMaxDepth));

    EXPECT_FALSE(javaOverflowDecision(kMaxSequence - 1, 1, kMaxSequence, kMaxDepth));
    EXPECT_TRUE(javaOverflowDecision(kMaxSequence, 1, kMaxSequence, kMaxDepth))
        << "exactly maxSequence events are recorded";
}

TEST(JavaParityLockTest, SpanEventPositionsAreReservedAtomically) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 512;
    constexpr int kTotal = kThreads * kPerThread;

    SpanData span_data("parity-op", 1000, 0);

    std::vector<std::vector<std::pair<int32_t, int32_t>>> claimed(kThreads);
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        claimed[t].reserve(kPerThread);
        threads.emplace_back([&, t] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kPerThread; ++i) {
                claimed[t].push_back(span_data.nextEventSequenceAndDepth());
            }
        });
    }
    while (ready.load(std::memory_order_relaxed) < kThreads) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    std::vector<int32_t> sequences;
    std::vector<int32_t> depths;
    sequences.reserve(kTotal);
    depths.reserve(kTotal);
    for (const auto& per_thread : claimed) {
        for (const auto& [sequence, depth] : per_thread) {
            sequences.push_back(sequence);
            depths.push_back(depth);
        }
    }
    ASSERT_EQ(sequences.size(), static_cast<size_t>(kTotal));

    std::sort(sequences.begin(), sequences.end());
    std::sort(depths.begin(), depths.end());
    for (int i = 0; i < kTotal; ++i) {
        // No duplicate and no gap: sorted, the reservations are exactly the
        // contiguous range each counter started from.
        EXPECT_EQ(sequences[i], i) << "sequences must be unique and contiguous from 0";
        EXPECT_EQ(depths[i], i + 1) << "depths must be unique and contiguous from 1";
    }
    EXPECT_EQ(span_data.getEventSequence(), kTotal);
    EXPECT_EQ(span_data.getEventDepth(), kTotal + 1);
}


// ===========================================================================
// Group 4 - async id sentinel
// ===========================================================================

TEST(JavaParityLockTest, AsyncIdSentinel) {
    EXPECT_EQ(NONE_ASYNC_ID, 0) << "Java: asyncId 0 means no async context";
}

// ===========================================================================
// Group 5 - propagation header names and transaction id format
// ===========================================================================

TEST(JavaParityLockTest, PropagationHeaderNames) {
    EXPECT_EQ(HEADER_TRACE_ID, "Pinpoint-TraceID");
    EXPECT_EQ(HEADER_SPAN_ID, "Pinpoint-SpanID");
    EXPECT_EQ(HEADER_PARENT_SPAN_ID, "Pinpoint-pSpanID");
    EXPECT_EQ(HEADER_SAMPLED, "Pinpoint-Sampled");
    EXPECT_EQ(HEADER_FLAG, "Pinpoint-Flags");
    EXPECT_EQ(HEADER_PARENT_APP_NAME, "Pinpoint-pAppName");
    EXPECT_EQ(HEADER_PARENT_APP_TYPE, "Pinpoint-pAppType");
    EXPECT_EQ(HEADER_PARENT_APP_NAMESPACE, "Pinpoint-pAppNamespace");
    EXPECT_EQ(HEADER_PARENT_SERVICE_NAME, "Pinpoint-pServiceName");
    EXPECT_EQ(HEADER_HOST, "Pinpoint-Host");
}

// The annotation keys the agent emits. The collector and the web tier read
// them by number.
TEST(JavaParityLockTest, AnnotationKeys) {
    EXPECT_EQ(ANNOTATION_API, 12);
    EXPECT_EQ(ANNOTATION_SQL_ID, 20);
    EXPECT_EQ(ANNOTATION_SQL_UID, 25);
    EXPECT_EQ(ANNOTATION_HTTP_URL, 40);
    EXPECT_EQ(ANNOTATION_HTTP_STATUS_CODE, 46);
    EXPECT_EQ(ANNOTATION_HTTP_PROXY_HEADER, 300);
    EXPECT_EQ(ANNOTATION_EXCEPTION_ID, -52) << "Java AnnotationKey.EXCEPTION_CHAIN_ID";
}

TEST(JavaParityLockTest, TransactionIdFormat) {
    const TraceId trace_id{std::string_view{"test-agent"}, 1600000000000LL, 42LL};
    EXPECT_EQ(trace_id.toString(), "test-agent^1600000000000^42");

    const auto parsed = TraceId::parseTraceId(trace_id.toString());
    ASSERT_FALSE(parsed.empty());
    EXPECT_EQ(parsed.agentId(), "test-agent");
    EXPECT_EQ(parsed.StartTime, 1600000000000LL);
    EXPECT_EQ(parsed.Sequence, 42LL);
}

TEST(JavaParityLockTest, TransactionIdParsing) {
    struct Case {
        const char* txid;
        bool ok;
    };
    const Case cases[] = {
        {"agent.id_-09^1^2", true},
        {"a^1^2^3", true},
        {"bad agent^1^2", false},
        {"bad/agent^1^2", false},
        {"^1^2", false},
        {"agent^1", false},
        {"agent^x^2", false},
        {"agent^1^x", false},
        {"", false},
    };
    for (const auto& c : cases) {
        EXPECT_EQ(!TraceId::parseTraceId(c.txid).empty(), c.ok)
            << "parseTraceId(\"" << c.txid << "\")";
    }

    const auto extra = TraceId::parseTraceId("a^1^2^3");
    ASSERT_FALSE(extra.empty());
    EXPECT_EQ(extra.agentId(), "a");
    EXPECT_EQ(extra.StartTime, 1LL);
    EXPECT_EQ(extra.Sequence, 2LL) << "the parser stops at the third delimiter";
}

TEST(JavaParityLockTest, SampledHeaderEncoding) {
    constexpr std::string_view kSamplingFlagFalse = "s0";

    EXPECT_EQ(kSamplingFlagFalse, "s0") << "the off value is exactly \"s0\"";
    for (const std::string_view value : {"s1", "S0", "", "0", "false", "s00", " s0"}) {
        EXPECT_NE(kSamplingFlagFalse, value) << "\"" << value << "\" must not disable sampling";
    }
}

TEST(JavaParityLockTest, ParentAppTypeDefaultsToUndefined) {
    auto span_data = std::make_shared<SpanData>("parity-op", 1000, 0);
    span_data->setTraceId(TraceId{std::string_view{"test-agent"}, 1600000000000LL, 7LL});
    span_data->setParentAppName("upstream");
    google::protobuf::Arena arena;
    const auto* child = build_grpc_span(std::make_unique<SpanChunk>(span_data, /*final=*/true), &arena);
    ASSERT_TRUE(child->has_acceptevent());
    ASSERT_TRUE(child->acceptevent().has_parentinfo());
    EXPECT_EQ(child->acceptevent().parentinfo().parentapplicationtype(), -1)
        << "no Pinpoint-pAppType: UNDEFINED, not UNKNOWN";
}

// ===========================================================================
// Group 6 - sampling formulas
// ===========================================================================

TEST(JavaParityLockTest, CountingSamplerPhase) {
    CounterSampler sampler(3);

    std::vector<int> sampled;
    for (int i = 1; i <= 10; ++i) {
        if (sampler.isSampled()) {
            sampled.push_back(i);
        }
    }
    EXPECT_EQ(sampled, (std::vector<int>{1, 4, 7, 10}))
        << "the first call and every 3rd after it";
}

TEST(JavaParityLockTest, PercentSamplerWindow) {
    PercentSampler sampler(1.0);  // rate 100 of 10000

    std::vector<int> sampled;
    for (int i = 1; i <= 200; ++i) {
        if (sampler.isSampled()) {
            sampled.push_back(i);
        }
    }
    EXPECT_EQ(sampled, (std::vector<int>{1, 101}))
        << "one per hundred, starting at the first call";
}

TEST(JavaParityLockTest, PercentSamplerEdgeRates) {
    EXPECT_EQ(MAX_PERCENT_RATE, 100 * 100) << "Java: 100 * 100";

    PercentSampler always(100.0);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(always.isSampled()) << "100% is the TrueSampler case";
    }

    PercentSampler clamped(150.0);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(clamped.isSampled()) << "over 100 is clamped to 100";
    }

    PercentSampler never(0.0);
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(never.isSampled()) << "0% is the FalseSampler case";
    }

    PercentSampler below_minimum(0.009);
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(below_minimum.isSampled()) << "a rate under 0.01 truncates to 0";
    }

    PercentSampler negative(-1.0);
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(negative.isSampled()) << "a negative percentage is clamped to 0";
    }
}

TEST(JavaParityLockTest, CountingSamplerEdgeRates) {
    CounterSampler always(1);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(always.isSampled()) << "rate 1 samples everything";
    }

    CounterSampler never(0);
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(never.isSampled()) << "rate 0 samples nothing";
    }

    CounterSampler negative(-7);
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(negative.isSampled())
            << "a negative rate must not be promoted to unsigned";
    }
}

namespace {

// The bucket reads time only through now_nanos(), so its schedule can be
// checked exactly and without sleeping. baseline_at re-does what the base
// constructor could not: it read the real clock, because virtual dispatch
// during construction does not reach this override. Same shape as
// test_limiter.cpp's FakeClockLimiter, kept local so this file does not
// depend on that one.
class ParityFakeClockLimiter final : public RateLimiter {
public:
    explicit ParityFakeClockLimiter(const uint64_t tps) : RateLimiter(tps) { baseline_at(now_ns_); }
    void advance_ms(const int64_t ms) { now_ns_ += ms * 1000000; }

protected:
    int64_t now_nanos() const override { return now_ns_; }

private:
    int64_t now_ns_{0};
};

int countAllowed(RateLimiter& limiter, const int calls) {
    int allowed = 0;
    for (int i = 0; i < calls; ++i) {
        if (limiter.allow()) {
            ++allowed;
        }
    }
    return allowed;
}

}  // namespace

TEST(JavaParityLockTest, ThroughputLimiterInitialState) {
    constexpr uint64_t kTps = 10;  // one token per 100ms

    // Called at once, the bucket is empty: the first call passes (Guava lets
    // the first caller borrow against the future) and the next has to wait a
    // whole token interval.
    ParityFakeClockLimiter immediate(kTps);
    EXPECT_TRUE(immediate.allow()) << "the first call passes";
    EXPECT_FALSE(immediate.allow()) << "and exactly one: no token is due yet";
    immediate.advance_ms(50);
    EXPECT_FALSE(immediate.allow()) << "half an interval is not a token";
    immediate.advance_ms(50);
    EXPECT_TRUE(immediate.allow()) << "one interval elapsed, one token due";

    // The refill clock starts at construction, not at the first call: an agent
    // that sits idle for a second owes its first caller a second of tokens.
    // Were the clock started lazily this would admit 1, the empty-bucket case
    // above. The +1 on top of the stored second is the token that comes due at
    // the instant of the call - the same one Guava lets through, and the one
    // test_limiter.cpp's LongIdleDoesNotAccumulate documents.
    ParityFakeClockLimiter idle_one_second(kTps);
    idle_one_second.advance_ms(1000);
    const int after_one_second = countAllowed(idle_one_second, 5 * static_cast<int>(kTps));
    EXPECT_EQ(after_one_second, static_cast<int>(kTps) + 1)
        << "one second of stored permits, plus the one due at the baseline";

    // Capacity is one second of permits: idling for ten releases no more than
    // idling for one - Guava's maxBurstSeconds = 1.
    ParityFakeClockLimiter idle_ten_seconds(kTps);
    idle_ten_seconds.advance_ms(10 * 1000);
    EXPECT_EQ(countAllowed(idle_ten_seconds, 5 * static_cast<int>(kTps)), after_one_second)
        << "ten idle seconds are worth no more than one: the bucket caps at a second";
}

// ===========================================================================
// Group 7 - URI histogram layout
// ===========================================================================

TEST(JavaParityLockTest, UrlStatHistogramBuckets) {
    EXPECT_EQ(URL_STATS_BUCKET_SIZE, 8);
    EXPECT_EQ(URL_STATS_BUCKET_VERSION, 0) << "Java UriStatHistogramBucket.getVersion";

    struct Case {
        int32_t elapsed;
        int bucket;
    };
    const Case cases[] = {
        {0, 0},       {99, 0},
        {100, 1},     {299, 1},
        {300, 2},     {499, 2},
        {500, 3},     {999, 3},
        {1000, 4},    {2999, 4},
        {3000, 5},    {4999, 5},
        {5000, 6},    {7999, 6},
        {8000, 7},    {1000000, 7},
    };
    for (const auto& c : cases) {
        UrlStatHistogram histogram;
        histogram.add(c.elapsed);
        for (int i = 0; i < URL_STATS_BUCKET_SIZE; ++i) {
            EXPECT_EQ(histogram.histogram(i), i == c.bucket ? 1 : 0)
                << "elapsed " << c.elapsed << " belongs in bucket " << c.bucket;
        }
    }
}

TEST(JavaParityLockTest, UrlStatWindow) {
    EXPECT_EQ(URL_STAT_TICK_INTERVAL, std::chrono::seconds(30)) << "Java TickClock interval";
    EXPECT_EQ(UrlStats(nullptr).sendInterval(), std::chrono::milliseconds(defaults::STAT_INTERVAL_MS))
        << "URL stats leave on the agent stat cadence, as Java's UriStatCollectingJob does";
    EXPECT_EQ(UrlStats::maxCompletedSnapshots(), 5u)
        << "Java snapshotQueue retains SNAPSHOT_LIMIT + 1 completed ticks";
    EXPECT_EQ(defaults::HTTP_URL_STAT_LIMIT, 1000)
        << "Java profiler.uri.stat.completed.data.limit.size (DefaultMonitorConfig)";

    // Two completions 30s apart fall in different ticks; anything inside the
    // same window shares one.
    const TickClock clock(std::chrono::duration_cast<std::chrono::seconds>(URL_STAT_TICK_INTERVAL).count());
    const auto base = std::chrono::system_clock::time_point{std::chrono::milliseconds(1600000020000LL)};

    const auto tick = clock.tick(base);
    EXPECT_EQ(tick % 30000, 0) << "a tick is aligned to the 30s epoch boundary";
    EXPECT_EQ(clock.tick(base + std::chrono::milliseconds(29999)), tick);
    EXPECT_EQ(clock.tick(base + std::chrono::milliseconds(30000)), tick + 30000);
}

TEST(JavaParityLockTest, UrlStatUnknownKey) {
    EXPECT_EQ(URL_STAT_UNKNOWN, "/NULL") << "Java URITemplate.NULL_URI";
}

TEST(JavaParityLockTest, UrlStatEmptyHistogram) {
    UrlStatHistogram histogram;
    EXPECT_TRUE(histogram.empty()) << "a fresh histogram is empty";

    histogram.add(0);
    EXPECT_FALSE(histogram.empty()) << "a 0ms sample lands in bucket 0 and is not empty";
    EXPECT_EQ(histogram.total(), 0);
    EXPECT_EQ(histogram.histogram(0), 1);
}


TEST(JavaParityLockTest, UrlStatEntryWithoutAnEndTimeIsSkipped) {
    UrlStatSnapshot snapshot;
    const Config config;
    TickClock tick_clock(1);

    UrlStatEntry no_end_time("/parity/api", "GET", 200);
    no_end_time.elapsed_ = 10;
    ASSERT_EQ(no_end_time.end_time_, std::chrono::system_clock::time_point{})
        << "a fresh entry carries no end time";

    EXPECT_TRUE(snapshot.add(&no_end_time, config, tick_clock))
        << "Java returns true too: a producer bug is not a capacity drop";
    EXPECT_TRUE(snapshot.empty()) << "no key under tick 0";
    EXPECT_EQ(snapshot.tick(), 0) << "and no watermark pinned in 1970";

    // A real entry alongside it is unaffected, and the skipped one does not
    // move the watermark back afterwards either.
    UrlStatEntry recorded("/parity/api", "GET", 200);
    recorded.elapsed_ = 10;
    recorded.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(700));
    EXPECT_TRUE(snapshot.add(&recorded, config, tick_clock));
    ASSERT_EQ(snapshot.getEachStats().size(), 1u);
    EXPECT_EQ(snapshot.tick(), tick_clock.tick(recorded.end_time_));

    EXPECT_TRUE(snapshot.add(&no_end_time, config, tick_clock));
    EXPECT_EQ(snapshot.getEachStats().size(), 1u);
    EXPECT_EQ(snapshot.tick(), tick_clock.tick(recorded.end_time_));
}

// ===========================================================================
// Group 8 - active trace histogram layout
// ===========================================================================

TEST(JavaParityLockTest, ActiveTraceHistogram) {
    constexpr int64_t kSampleTimeMs = 100000;

    struct Case {
        int64_t elapsed_ms;
        int slot;
    };
    const Case cases[] = {
        {0, 0},     {1000, 0},
        {1001, 1},  {3000, 1},
        {3001, 2},  {5000, 2},
        {5001, 3},  {60000, 3},
    };

    for (const auto& c : cases) {
        ActiveSpanRegistry registry;
        ActiveSpanNode node;
        registry.add(node, /*span_id=*/1, kSampleTimeMs - c.elapsed_ms);

        int32_t buckets[4]{};
        registry.collect(buckets, kSampleTimeMs);
        for (int i = 0; i < 4; ++i) {
            EXPECT_EQ(buckets[i], i == c.slot ? 1 : 0)
                << "an active span of " << c.elapsed_ms << "ms belongs in slot " << c.slot;
        }

        registry.drop(node);
    }
}


// ===========================================================================
// Group 10 - message truncation format
// ===========================================================================

TEST(JavaParityLockTest, TruncationFormat) {
    EXPECT_EQ(abbreviateString("short", 10), "short") << "a value within the limit is untouched";
    EXPECT_EQ(abbreviateString("0123456789", 10), "0123456789") << "exactly at the limit is untouched";
    EXPECT_EQ(abbreviateString("0123456789A", 10), "0123456789...(11)")
        << "the marker carries the original length";
}

TEST(JavaParityLockTest, TruncationCutsOnAUtf8Boundary) {
    // U+AC00 is three bytes; a limit of 4 lands inside the second character.
    const std::string source = "\xEA\xB0\x80\xEA\xB0\x80\xEA\xB0\x80";
    ASSERT_EQ(source.size(), 9u);

    const auto abbreviated = abbreviateString(source, 4);
    EXPECT_EQ(abbreviated, "\xEA\xB0\x80...(9)");
    EXPECT_TRUE(isValidUtf8(abbreviated)) << "the result must stay valid UTF-8 for protobuf";
}

TEST(JavaParityLockTest, MessageLimits) {
    EXPECT_EQ(kMaxErrorStringLength, 256u) << "Java AbstractRecorder.recordException";
    EXPECT_EQ(kMaxSqlMetaLength, 64u * 1024u) << "Java profiler.jdbc.maxsqllength";
}

// ===========================================================================
// Group 11 - gRPC channel constants
// ===========================================================================

// The three collector ports.
TEST(JavaParityLockTest, CollectorPortDefaults) {
    EXPECT_EQ(defaults::AGENT_PORT, 9991) << "Java profiler.transport.grpc.agent.collector.port";
    EXPECT_EQ(defaults::STAT_PORT, 9992) << "Java profiler.transport.grpc.stat.collector.port";
    EXPECT_EQ(defaults::SPAN_PORT, 9993) << "Java profiler.transport.grpc.span.collector.port";
}

TEST(JavaParityLockTest, GrpcChannelDefaults) {
    EXPECT_EQ(defaults::GRPC_KEEPALIVE_TIME_MS, 30 * 1000) << "Java ClientOption keepAliveTime";
    EXPECT_EQ(defaults::GRPC_KEEPALIVE_TIMEOUT_MS, 60 * 1000) << "Java ClientOption keepAliveTimeout";
    EXPECT_EQ(defaults::GRPC_MAX_MESSAGE_SIZE, 4 * 1024 * 1024) << "Java ClientOption maxInboundMessageSize";
    EXPECT_EQ(defaults::GRPC_CHANNEL_MAX_AGE_MS, 0) << "renewal off, as in Java";
    EXPECT_EQ(defaults::GRPC_STREAM_MAX_AGE_MS, 0) << "renewal off, as in Java";
    EXPECT_EQ(defaults::GRPC_IDLE_TIMEOUT_MS, 0) << "idle timeout off, as Java's IDLE_TIMEOUT_MILLIS_DISABLE";
}

TEST(JavaParityLockTest, AgentInfoSchedule) {
    EXPECT_EQ(defaults::AGENT_INFO_REFRESH_INTERVAL_MS, 24 * 60 * 60 * 1000)
        << "Java AgentInfoSender refresh interval";
    EXPECT_EQ(defaults::AGENT_INFO_MAX_TRY_PER_ATTEMPT, 3)
        << "Java AgentInfoSender maxTryPerAttempt";
    EXPECT_EQ(defaults::AGENT_INFO_SEND_RETRY_INTERVAL_MS, 3000)
        << "matches the Go agent, not Java's effective 300000ms - see doc/java_parity.md";
}

TEST(JavaParityLockTest, SpanBatchDefaults) {
    EXPECT_EQ(defaults::SPAN_BATCH_SIZE, 20) << "Java span.batch-sender.size";
    EXPECT_EQ(defaults::SPAN_BATCH_FLUSH_INTERVAL_MS, 1000);
    EXPECT_EQ(defaults::SPAN_BATCH_COLLECT_DEADLINE_MS, 500);
    EXPECT_EQ(defaults::SPAN_BATCH_MAX_CONCURRENT_REQUESTS, 10);
}

TEST(JavaParityLockTest, StatCollectionDefaults) {
    EXPECT_EQ(defaults::STAT_INTERVAL_MS, 5000) << "Java DefaultMonitorConfig code default";
    EXPECT_EQ(defaults::STAT_BATCH_COUNT, 6) << "Java profiler.jvm.stat.batch.send.count";
}

TEST(JavaParityLockTest, SqlCacheDefaults) {
    EXPECT_EQ(defaults::SQL_CACHE_SIZE, 1024) << "Java profiler.jdbc.sqlcachesize";
    EXPECT_EQ(defaults::SQL_CACHE_LENGTH_LIMIT, 2048) << "Java profiler.jdbc.sqlcachelengthlimit";
    EXPECT_EQ(defaults::SQL_CACHE_EXPIRE_HOURS, 168) << "Java profiler.jdbc.sqlcacheexpirehours";
    EXPECT_EQ(defaults::SQL_MAX_BIND_ARGS_SIZE, 1024) << "Java profiler.jdbc.maxsqlbindvaluesize";
    EXPECT_EQ(defaults::SQL_ERROR_COUNT, 100) << "Java profiler.sql.error.count";
}

TEST(JavaParityLockTest, SqlCacheLengthLimitAppliesToTheUidCacheOnly) {
    constexpr size_t kLengthLimit = 16;
    const std::string under_limit(kLengthLimit - 1, 'a');
    const std::string at_limit(kLengthLimit, 'b');

    SqlUidCache uid_cache(/*max_size=*/64, kDefaultCacheShardCount, kLengthLimit);
    EXPECT_FALSE(uid_cache.bypasses(under_limit));
    EXPECT_TRUE(uid_cache.bypasses(at_limit)) << "Java stores only key.length() < bypassLength";

    // A bypassed statement is never "found", so its UID metadata is published
    // again on every use; a short one is cached after the first miss.
    EXPECT_FALSE(uid_cache.get(at_limit).found);
    EXPECT_FALSE(uid_cache.get(at_limit).found) << "still a miss on the second lookup";
    EXPECT_FALSE(uid_cache.get(under_limit).found);
    EXPECT_TRUE(uid_cache.get(under_limit).found) << "a short statement is stored";

    // The id cache takes no limit and caches a statement of any length: the id
    // is a counter, so re-minting one for a long statement would leak ids and
    // re-publish PSqlMetaData forever.
    IdCache id_cache(/*max_size=*/64);
    const std::string very_long(kLengthLimit * 100, 'c');
    const auto first = id_cache.get(very_long);
    EXPECT_FALSE(first.found) << "first sight of a statement is a miss";
    const auto second = id_cache.get(very_long);
    EXPECT_TRUE(second.found) << "Java newSqlCache() passes no bypass length";
    EXPECT_EQ(first.value, second.value) << "and the id is stable";
}

// ===========================================================================
// Group 12 - error cause categories
// ===========================================================================

TEST(JavaParityLockTest, ErrorCategoryBitValues) {
    EXPECT_EQ(static_cast<int>(ErrorCategory::kUnknown), 1 << 0) << "Java ErrorCategory.UNKNOWN";
    EXPECT_EQ(static_cast<int>(ErrorCategory::kException), 1 << 1) << "Java ErrorCategory.EXCEPTION";
    EXPECT_EQ(static_cast<int>(ErrorCategory::kHttpStatus), 1 << 2) << "Java ErrorCategory.HTTP_STATUS";
    EXPECT_EQ(static_cast<int>(ErrorCategory::kSql), 1 << 3) << "Java ErrorCategory.SQL";
    EXPECT_EQ(ALL_ERROR_CATEGORIES, 15) << "kUnknown|kException|kHttpStatus|kSql";
}

TEST(JavaParityLockTest, ErrorMarkMaskResolution) {
    constexpr int kUnknown = static_cast<int>(ErrorCategory::kUnknown);
    constexpr int kException = static_cast<int>(ErrorCategory::kException);
    constexpr int kHttpStatus = static_cast<int>(ErrorCategory::kHttpStatus);
    constexpr int kSql = static_cast<int>(ErrorCategory::kSql);

    EXPECT_EQ(error_mark_mask({}, {}), ALL_ERROR_CATEGORIES)
        << "Java: a null errorMarkString is EnumSet.allOf";
    EXPECT_EQ(error_mark_mask({"exception"}, {}), kUnknown | kException)
        << "a non-empty mark is the whole allow-list";
    EXPECT_EQ(error_mark_mask({}, {"http-status"}), ALL_ERROR_CATEGORIES & ~kHttpStatus)
        << "exclude subtracts from the default everything";
    EXPECT_EQ(error_mark_mask({"exception", "sql"}, {"sql"}), kUnknown | kException)
        << "exclude wins over mark (Java mark.removeAll(exclude))";

    // kUnknown survives every combination, including one that names it: it has
    // no spelling of its own, so the token is simply unrecognized and ignored,
    // and the bit is OR-ed back in afterwards.
    EXPECT_EQ(error_mark_mask({"exception"}, {"exception"}), kUnknown)
        << "Java mark.add(ErrorCategory.UNKNOWN) runs after removeAll";
    EXPECT_EQ(error_mark_mask({}, {"unknown"}), ALL_ERROR_CATEGORIES)
        << "UNKNOWN is not selectable and cannot be excluded";
    EXPECT_EQ(error_mark_mask({""}, {}), kUnknown)
        << "Java: an empty token is skipped, leaving an empty (not default) set";

    EXPECT_EQ(error_mark_mask({"EXCEPTION", " Http-Status ", "sQl"}, {}), ALL_ERROR_CATEGORIES);
    EXPECT_EQ(error_mark_mask({"exception,sql"}, {}), kUnknown | kException | kSql);

    EXPECT_EQ(error_mark_mask({"exception", "nonsense"}, {}), kUnknown | kException);
    EXPECT_EQ(error_mark_mask({}, {"nonsense"}), ALL_ERROR_CATEGORIES);

    // Every category excluded still leaves kUnknown, which is what stops the
    // two keys from adding up to "never fail a transaction".
    EXPECT_EQ(error_mark_mask({}, {"exception", "http-status", "sql"}), kUnknown);
}


// ===========================================================================
// Group 13 - queue overflow policy
// ===========================================================================

TEST(JavaParityLockTest, SpanQueueHeadDropsTheOldest) {
    constexpr size_t kCapacity = 4;
    constexpr int kEnqueued = 10;

    ShardedBoundedQueue<std::unique_ptr<int>> queue(kCapacity, /*requested_shards=*/1);
    ASSERT_EQ(queue.capacity(), kCapacity);
    ASSERT_EQ(queue.shard_count(), 1u) << "one shard, so retention order is the global one";

    for (int value = 0; value < kEnqueued; ++value) {
        auto item = std::make_unique<int>(value);
        queue.enqueue(item);
        EXPECT_EQ(item, nullptr) << "enqueue always takes the value: nothing is refused";
    }

    EXPECT_EQ(queue.dropped_oldest(), static_cast<uint64_t>(kEnqueued) - kCapacity)
        << "every overflow is counted, as Java's discard-oldest log line is";

    std::vector<std::unique_ptr<int>> drained;
    EXPECT_EQ(queue.try_dequeue_batch(drained, 2 * kCapacity), kCapacity);

    std::vector<int> survivors;
    survivors.reserve(drained.size());
    for (const auto& value : drained) {
        survivors.push_back(*value);
    }
    EXPECT_EQ(survivors, (std::vector<int>{6, 7, 8, 9}))
        << "the newest survive; head-drop discards the oldest, never the arrival";
}

// ===========================================================================
// Group 14 - proxy request header pipeline
// ===========================================================================

namespace {

class ParityHeaderReader final : public HeaderReader {
public:
    explicit ParityHeaderReader(std::map<std::string, std::string> headers)
        : headers_(std::move(headers)) {}

    std::optional<std::string_view> Get(std::string_view key) const override {
        const auto it = headers_.find(std::string{key});
        if (it == headers_.end()) {
            return std::nullopt;
        }
        return std::string_view{it->second};
    }

    void ForEach(std::function<bool(std::string_view, std::string_view)> callback) const override {
        for (const auto& [key, value] : headers_) {
            if (!callback(key, value)) {
                break;
            }
        }
    }

private:
    std::map<std::string, std::string> headers_;
};

// Runs the whole proxy pipeline over one request and hands back the proxy
// annotations it recorded, in order.
std::vector<LongIntIntByteByteStringValue> recordProxyHeaders(
        std::map<std::string, std::string> headers,
        const std::vector<std::string>& user_header_names = {}) {
    const ParityHeaderReader reader(std::move(headers));
    PinpointAnnotation annotation;
    HttpTracerUtil::setProxyHeader(reader, &annotation, user_header_names);

    std::vector<LongIntIntByteByteStringValue> recorded;
    for (const auto& [key, data] : annotation.getAnnotations()) {
        if (key == ANNOTATION_HTTP_PROXY_HEADER) {
            recorded.push_back(std::get<LongIntIntByteByteStringValue>(data.data));
        }
    }
    return recorded;
}

}  // namespace

TEST(JavaParityLockTest, ProxyParsersRunIndependently) {
    const auto recorded = recordProxyHeaders({
        {"Pinpoint-ProxyApache", "t=1000000000000 D=100 i=5 b=95"},
        {"Pinpoint-ProxyNginx", "t=2000000.000 D=0.200"},
        {"Pinpoint-ProxyApp", "t=3000000000000 app=OtherApp"},
        {"Pinpoint-ProxyUser", "t=4000000000000 D=300"},
    }, {"Pinpoint-ProxyUser"});

    std::vector<int32_t> codes;
    codes.reserve(recorded.size());
    for (const auto& value : recorded) {
        codes.push_back(value.intValue1);
    }
    std::sort(codes.begin(), codes.end());
    EXPECT_EQ(codes, (std::vector<int32_t>{1, 2, 3, 4}))
        << "app=1, nginx=2, apache=3, user=4 - one annotation per valid header";
}

TEST(JavaParityLockTest, ProxyHeaderNeedsAPositiveReceivedTime) {
    struct Case {
        const char* header;
        const char* value;
    };
    const Case cases[] = {
        {"Pinpoint-ProxyApache", "D=1500 i=10 b=90"},  // no t= at all
        {"Pinpoint-ProxyNginx", "t=0.000 D=0.123"},    // t= present, not positive
        {"Pinpoint-ProxyApp", "t=abc app=MyApp"},      // t= present, unparseable
        {"Pinpoint-ProxyUser", "t=0000000000000 D=1500"},
    };

    for (const auto& c : cases) {
        const auto recorded = recordProxyHeaders({{c.header, c.value}}, {"Pinpoint-ProxyUser"});
        EXPECT_TRUE(recorded.empty())
            << c.header << ": '" << c.value << "' must record no annotation";
    }
}

TEST(JavaParityLockTest, ProxyNginxTimestampsAreExactThreeDecimals) {
    const auto recorded = recordProxyHeaders(
        {{"Pinpoint-ProxyNginx", "t=1504230492.763 D=0.123"}});
    ASSERT_EQ(recorded.size(), 1u);
    EXPECT_EQ(recorded[0].longValue, 1504230492763) << "sec.mmm recorded as milliseconds";
    EXPECT_EQ(recorded[0].intValue2, 123000) << "0.123s is exactly 123000us, not 122999";

    // Anything but sec.mmm records no duration rather than a guess - the
    // plain microsecond integer apache sends included, which read as seconds
    // would inflate the duration a millionfold.
    for (const char* d_val : {"0.1", "0.12", "0.1234", "123", "-0.123"}) {
        const auto other = recordProxyHeaders(
            {{"Pinpoint-ProxyNginx", std::string("t=1504230492.763 D=") + d_val}});
        ASSERT_EQ(other.size(), 1u) << d_val << " must not discard the header";
        EXPECT_EQ(other[0].intValue2, -1) << "D=" << d_val << " is not sec.mmm: unset (-1)";
    }

    // The same format rule on `t=`, where failing it drops the header.
    for (const char* t_val : {"1504230492.76", "1504230492", "1504230492.7634"}) {
        EXPECT_TRUE(recordProxyHeaders(
                        {{"Pinpoint-ProxyNginx", std::string("t=") + t_val + " D=0.123"}})
                        .empty())
            << "t=" << t_val << " is not sec.mmm";
    }
}

TEST(JavaParityLockTest, ProxyUserHeaderInfersItsWriter) {
    struct Case {
        const char* value;
        int64_t received_time;
        int32_t duration;
    };
    for (const Case c : {
             Case{"t=1504230492763123 D=1500", 1504230492763LL, 1500},
             Case{"t=1504230492.763 D=0.123", 1504230492763LL, 123000},
             Case{"t=1504230492763 D=42", 1504230492763LL, 42},
             Case{"t=1504230492763 D=-5", 1504230492763LL, -1},
             Case{"t=1504230492763 D=-0.123", 1504230492763LL, -1},
             Case{"t=1504230492763 D=3000000.000", 1504230492763LL, -1},
             Case{"t=1504230492763", 1504230492763LL, -1},
         }) {
        const auto recorded = recordProxyHeaders({{"X-Proxy-Time", c.value}}, {"X-Proxy-Time"});
        ASSERT_EQ(recorded.size(), 1u) << c.value;
        EXPECT_EQ(recorded[0].longValue, c.received_time) << c.value;
        EXPECT_EQ(recorded[0].intValue2, c.duration) << c.value;
        EXPECT_EQ(recorded[0].stringValue, "X-Proxy-Time") << "the header name is the app";
    }

    for (const char* bad : {"150423049276", "15042304.9276", "1504230492.76", "abc"}) {
        EXPECT_TRUE(recordProxyHeaders({{"X-Proxy-Time", std::string("t=") + bad}}, {"X-Proxy-Time"}).empty())
            << "t=" << bad << " fits no proxy's format";
    }
}

TEST(JavaParityLockTest, ProxyDurationAndPercentAreGated) {
    auto nginx = recordProxyHeaders({{"Pinpoint-ProxyNginx", "t=1504230492.763 D=-0.123"}});
    ASSERT_EQ(nginx.size(), 1u);
    EXPECT_EQ(nginx[0].intValue2, -1) << "negative nginx D= is unset";

    nginx = recordProxyHeaders({{"Pinpoint-ProxyNginx", "t=1504230492.763 D=3000000.000"}});
    ASSERT_EQ(nginx.size(), 1u);
    EXPECT_EQ(nginx[0].intValue2, -1) << "nginx D= past int32/1000 is unset, not wrapped";

    auto apache = recordProxyHeaders({{"Pinpoint-ProxyApache", "t=1504230492763123 D=-7 i=101 b=-1"}});
    ASSERT_EQ(apache.size(), 1u);
    EXPECT_EQ(apache[0].intValue2, -1) << "negative apache D= is unset";
    EXPECT_EQ(apache[0].byteValue1, -1) << "i= above 100 is unset";
    EXPECT_EQ(apache[0].byteValue2, -1) << "b= below 0 is unset";

    apache = recordProxyHeaders({{"Pinpoint-ProxyApache", "t=1504230492763123 D=7 i=0 b=100"}});
    ASSERT_EQ(apache.size(), 1u);
    EXPECT_EQ(apache[0].intValue2, 7);
    EXPECT_EQ(apache[0].byteValue1, 0);
    EXPECT_EQ(apache[0].byteValue2, 100);
}

TEST(JavaParityLockTest, ParentInfoOnlyWhenParentAppNameIsPresent) {
    const auto build = [](const std::string_view parent_app_name) {
        auto span_data = std::make_shared<SpanData>("parity-op", 1000, 0);
        span_data->setTraceId(TraceId{std::string_view{"test-agent"}, 1600000000000LL, 7LL});
        span_data->setEndPoint("api.example.com:8080");
        span_data->setRemoteAddr("10.0.0.1:54321");
        // Set on both paths: an acceptor host alone must not produce a parent.
        span_data->setAcceptorHost("api.example.com:8080");
        if (!parent_app_name.empty()) {
            span_data->setParentAppName(parent_app_name);
            span_data->setParentAppType(1010);
            span_data->setParentServiceName("parent-service");
        }
        return std::make_unique<SpanChunk>(span_data, /*final=*/true);
    };

    google::protobuf::Arena arena;

    const auto* orphan = build_grpc_span(build(""), &arena);
    ASSERT_TRUE(orphan->has_acceptevent());
    EXPECT_FALSE(orphan->acceptevent().has_parentinfo())
        << "an acceptor host on its own must not invent a parent node";

    const auto* child = build_grpc_span(build("ParentApp"), &arena);
    ASSERT_TRUE(child->has_acceptevent());
    ASSERT_TRUE(child->acceptevent().has_parentinfo());
    EXPECT_EQ(child->acceptevent().parentinfo().parentapplicationname(), "ParentApp");
    EXPECT_EQ(child->acceptevent().parentinfo().parentapplicationtype(), 1010);
    EXPECT_EQ(child->acceptevent().parentinfo().acceptorhost(), "api.example.com:8080");
    EXPECT_EQ(child->acceptevent().parentinfo().parentservicename(), "parent-service");
}

// ===========================================================================
// Group 15 - logging level policy
// ===========================================================================

TEST(JavaParityLockTest, UnsupportedLogLevelKeepsTheCurrentLevel) {
    auto& logger = Logger::getInstance();

    logger.setLogLevel("error");
    ASSERT_TRUE(logger.errorEnabled());
    ASSERT_FALSE(logger.warnEnabled()) << "error suppresses warn";

    // Matching is case-insensitive but NOT trimmed, so "WARN " is unsupported
    // where "WARN" would be a level change.
    for (const char* unsupported : {"warnign", "trace", "fatal", "", "WARN ", "2"}) {
        logger.setLogLevel(unsupported);
        EXPECT_TRUE(logger.errorEnabled());
        EXPECT_FALSE(logger.warnEnabled())
            << "'" << unsupported << "' must leave the level where it was";
    }

    // Both spellings of warn are accepted, and each is a real level change.
    logger.setLogLevel("warn");
    EXPECT_TRUE(logger.warnEnabled()) << "'warn' is accepted";
    EXPECT_FALSE(logger.infoEnabled());

    logger.setLogLevel("error");
    ASSERT_FALSE(logger.warnEnabled());
    logger.setLogLevel("warning");
    EXPECT_TRUE(logger.warnEnabled()) << "'warning' is accepted";
    EXPECT_FALSE(logger.infoEnabled());

    logger.setLogLevel("info");  // back to the agent default for the rest of the suite
    EXPECT_TRUE(logger.infoEnabled());
    EXPECT_FALSE(logger.debugEnabled());
}

TEST(JavaParityLockTest, LogRotationDefaults) {
    EXPECT_EQ(defaults::LOG_MAX_BACKUPS, 1);
    EXPECT_EQ(defaults::LOG_MAX_FILE_SIZE_MB, 10) << "10 MB before rotation";

    AgentOptions options;
    options.config_yaml =
        "ApplicationName: ParityLockApp\n"
        "Log:\n"
        "  MaxBackups: 0\n";
    const auto config = make_config(options, nullptr);
    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config->log.max_backups, defaults::LOG_MAX_BACKUPS)
        << "a value below 1 is restored to the default, not honoured";
    EXPECT_EQ(config->log.max_file_size, defaults::LOG_MAX_FILE_SIZE_MB)
        << "and an unset MaxFileSize stays at 10 MB";
}


TEST(JavaParityLockTest, MetadataRetryBudget) {
    const GrpcClientTuning tuning{};
    EXPECT_EQ(tuning.meta_retry_max_attempts, 3)
        << "Java profiler.transport.grpc.metadata.sender.retry.max.count";
    EXPECT_EQ(tuning.meta_retry_delay, std::chrono::milliseconds(1000))
        << "Java profiler.transport.grpc.metadata.sender.retry.delay.millis";
    EXPECT_EQ(tuning.meta_retry_queue_size, 1000u)
        << "port consensus: the retry schedule is bounded like the new-metadata queue "
           "(Java metadata.sender.executor.queue.size)";
    EXPECT_EQ(Config{}.collector.grpc.channel.sender_queue_size, 1000)
        << "Java profiler.transport.grpc.metadata.sender.executor.queue.size";
}

}  // namespace pinpoint
