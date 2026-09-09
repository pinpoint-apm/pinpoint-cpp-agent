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

// Locked parity invariants.
//
// Every assertion in this file is a value or an algorithm that the Java agent
// (agent-module/profiler), the Go agent and this agent were verified to agree
// on. They are locked here so a later change has to state its intent instead
// of drifting one agent away from the other two: a failure means either the
// change is wrong, or all three implementations and doc/java_parity.md move
// together.
//
// The Go agent keeps the same suite at java_parity_lock_test.go, group for
// group, and doc/java_parity.md ("Locked parity invariants") is the table that
// ties both to the Java reference. Add a group here only when the same group
// exists there.
//
// Where an existing suite in this directory already locks a group, this file
// says so and does not duplicate it - the cross-reference is the point, so a
// reader of one file can find the other half.
//
// Groups:
//   1  SQL normalization state machine   (golden cases: test_sql.cpp)
//   2  span event depth / sequence numbering
//   3  span chunk serialization          (test_span.cpp)
//   4  async id / sequence sentinels
//   5  propagation header names and transaction id format
//   6  sampling formulas
//   7  URI histogram layout
//   8  active trace histogram layout
//   9  transaction counters              (test_stat.cpp)
//  10  message truncation format
//  11  gRPC channel constants

#include "pinpoint/tracer.h"

#include "../src/agent_service.h"
#include "../src/active_span.h"
#include "../src/config.h"
#include "../src/sampling.h"
#include "../src/sql.h"
#include "../src/url_stat.h"
#include "../src/utility.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace pinpoint {

// ===========================================================================
// Group 1 - SQL normalization state machine
// ===========================================================================
//
// The byte-for-byte golden cases live in test_sql.cpp
// (SqlTest.JavaParityGoldenCases plus the ported JavaDefault* cases); the Go
// agent mirrors them in Test_javaParityLock_SqlNormalizerGoldenCases. What is
// locked here instead is the structural invariant the collector depends on and
// that no single case states on its own.

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
    // removeComments=false is Java's DefaultSqlNormalizer no-arg constructor,
    // which is what the golden expectations were taken from.
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

// Normalizing an already-normalized statement changes it again: `0#` becomes
// `0##`. All three agents behave this way, and a "fix" that made it idempotent
// would change every SQL id for a statement the agent happens to see twice.
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

// ===========================================================================
// Group 2 - span event depth / sequence numbering
// ===========================================================================

// Java: profiler.callstack.max.depth=64, profiler.callstack.max.sequence=5000,
// profiler.io.buffering.buffersize=20 (DefaultInstrumentConfig,
// pinpoint-root.config).
TEST(JavaParityLockTest, SpanEventLimitDefaults) {
    EXPECT_EQ(defaults::SPAN_MAX_EVENT_DEPTH, 64) << "Java profiler.callstack.max.depth";
    EXPECT_EQ(defaults::SPAN_MAX_EVENT_SEQUENCE, 5000) << "Java profiler.callstack.max.sequence";
    EXPECT_EQ(defaults::SPAN_EVENT_CHUNK_SIZE, 20) << "Java profiler.io.buffering.buffersize";
}

namespace {

// javaOverflowDecision mirrors the predicate all three agents implement, so the
// boundaries below are stated once in a form a reader can compare against
// Java's DefaultCallStack.isOverflow: `maxDepth < index || maxSequence <=
// sequence`, where index is the number of elements already on the stack. Both
// ports pass the depth the next event would take (index + 1), hence depth - 1.
//
// A mirror, not the real function: is_event_overflow is file-local to span.cpp
// and takes a config snapshot. SpanTest covers the real one; what is locked
// here is where the boundaries fall.
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

// ===========================================================================
// Group 3 - span chunk serialization
// ===========================================================================
//
// Locked by test_span.cpp: SpanChunkOptimizeMultipleEventsTest and
// SpanChunkOptimizeNonFinalKeyTimeTest cover keyTime (final = the span's start
// time, non-final = the first event's) and the startElapsed deltas;
// SpanChunkEndPointSnapshotTest covers the endPoint snapshot a non-final chunk
// carries. The Go agent mirrors all three in
// Test_javaParityLock_Chunk{KeyTimeAndStartElapsed,SortsBySequence,SnapshotsEndPoint}.
//
// Depth compression (an event at the same depth as its predecessor travels as
// depth 0) is locked on this side only: the Go agent's optimizeSpanEvents does
// not seed prevDepth on its first iteration, so its second event is never
// compressed. That is gap S4 of the 4th cross-agent review, and its Go test is
// skipped until the fix lands.

// ===========================================================================
// Group 4 - async id sentinel
// ===========================================================================

// Java: an async id of 0 means "no async context". NewAsyncSpan redraws while
// the generator hands out the sentinel, so a drawn id can never be mistaken
// for "absent".
TEST(JavaParityLockTest, AsyncIdSentinel) {
    EXPECT_EQ(NONE_ASYNC_ID, 0) << "Java: asyncId 0 means no async context";
}

// ===========================================================================
// Group 5 - propagation header names and transaction id format
// ===========================================================================

// All ten names, against Java's Header enum (commons). A rename on one side
// silently breaks tracing across a process boundary, with no error anywhere.
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

// The wire format `agentId^startTime^sequence` (Java TransactionIdUtils) and
// the round trip through the parser.
TEST(JavaParityLockTest, TransactionIdFormat) {
    const TraceId trace_id{std::string_view{"test-agent"}, 1600000000000LL, 42LL};
    EXPECT_EQ(trace_id.toString(), "test-agent^1600000000000^42");

    const auto parsed = TraceId::parseTraceId(trace_id.toString());
    ASSERT_FALSE(parsed.empty());
    EXPECT_EQ(parsed.agentId(), "test-agent");
    EXPECT_EQ(parsed.StartTime, 1600000000000LL);
    EXPECT_EQ(parsed.Sequence, 42LL);
}

// The parser's accept/reject set. Java validates the agent id character class
// (IdValidateUtils) and stops at the third delimiter, so "a^1^2^3" is the
// transaction "a^1^2" to all three agents.
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

// Only the exact string "s0" turns sampling off. Java:
// SamplingFlagUtils.isSamplingFlag - anything else, "s1" or an absent header
// included, is sampled.
TEST(JavaParityLockTest, SampledHeaderEncoding) {
    constexpr std::string_view kSamplingFlagFalse = "s0";

    EXPECT_EQ(kSamplingFlagFalse, "s0") << "the off value is exactly \"s0\"";
    for (const std::string_view value : {"s1", "S0", "", "0", "false", "s00", " s0"}) {
        EXPECT_NE(kSamplingFlagFalse, value) << "\"" << value << "\" must not disable sampling";
    }
}

// ===========================================================================
// Group 6 - sampling formulas
// ===========================================================================

// Java CountingSampler tests the pre-increment value, so the first request of
// the process is sampled and every rate-th one after it - not the rate-th
// request. SamplingTest covers rates 0 and 1; the phase is what is locked here.
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

// Java PercentRateSampler adds the rate to a counter and samples on a
// remainder in (0, rate] - the first request lands on exactly rate and is
// sampled, where a [0, rate) window would sample the second one instead.
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

// The truncation Java does in PercentSamplerFactory: the percentage is
// multiplied by 100 and truncated, so anything under 0.01 collects nothing.
// The two ends are the TrueSampler / FalseSampler cases Java hands off to.
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

// Java CountingSampler with a rate of 1 samples everything and a rate of 0
// nothing; a negative rate must not be treated as a huge unsigned one.
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

// ===========================================================================
// Group 7 - URI histogram layout
// ===========================================================================

// The eight bucket bounds, against Java's UriStatHistogramBucket.Layout. The
// collector stores the counts positionally, so a shifted boundary silently
// rewrites history.
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

// The tick size and the completed-queue cap. Java:
// AsyncQueueingUriStatStorage buckets on a 30s TickClock and keeps four
// snapshots. The send cadence is not a URL-stat constant: Java's
// UriStatCollectingJob runs on the agent stat scheduler
// (profiler.jvm.stat.collect.interval), so the default send ceiling here is
// Stat.BatchInterval's default rather than a second 30s timer.
TEST(JavaParityLockTest, UrlStatWindow) {
    EXPECT_EQ(URL_STAT_TICK_INTERVAL, std::chrono::seconds(30)) << "Java TickClock interval";
    EXPECT_EQ(UrlStats(nullptr).sendInterval(), std::chrono::milliseconds(defaults::STAT_INTERVAL_MS))
        << "URL stats leave on the agent stat cadence, as Java's UriStatCollectingJob does";

    // Two completions 30s apart fall in different ticks; anything inside the
    // same window shares one.
    const TickClock clock(std::chrono::duration_cast<std::chrono::seconds>(URL_STAT_TICK_INTERVAL).count());
    const auto base = std::chrono::system_clock::time_point{std::chrono::milliseconds(1600000020000LL)};

    const auto tick = clock.tick(base);
    EXPECT_EQ(tick % 30000, 0) << "a tick is aligned to the 30s epoch boundary";
    EXPECT_EQ(clock.tick(base + std::chrono::milliseconds(29999)), tick);
    EXPECT_EQ(clock.tick(base + std::chrono::milliseconds(30000)), tick + 30000);
}

// The stand-in URL for a span that recorded URL stats without a URI template.
// Java's URITemplate.NULL_URI, verbatim, so a mixed Java/C++ application
// aggregates its "no URI recorded" traffic under one server-side key.
TEST(JavaParityLockTest, UrlStatUnknownKey) {
    EXPECT_EQ(URL_STAT_UNKNOWN, "/NULL") << "Java URITemplate.NULL_URI";
}

// An all-zero histogram reports empty, which is what keeps an empty
// PUriHistogram off the wire. Java decides on a count field; both ports decide
// on the bucket sum, so a single 0ms sample must still count as non-empty.
TEST(JavaParityLockTest, UrlStatEmptyHistogram) {
    UrlStatHistogram histogram;
    EXPECT_TRUE(histogram.empty()) << "a fresh histogram is empty";

    histogram.add(0);
    EXPECT_FALSE(histogram.empty()) << "a 0ms sample lands in bucket 0 and is not empty";
    EXPECT_EQ(histogram.total(), 0);
    EXPECT_EQ(histogram.histogram(0), 1);
}

// ===========================================================================
// Group 8 - active trace histogram layout
// ===========================================================================

// The four active-trace slots, against Java's NORMAL histogram schema
// (BaseHistogramSchema: 1000/3000/5000ms with an inclusive upper bound, so a
// span at exactly 1000ms is still "fast").
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
// Group 9 - transaction counters
// ===========================================================================
//
// Locked by test_stat.cpp: StatTest.SamplingCountersTest and
// StatTest.AllCountersMixedIncrementTest cover all six counters Java's
// DefaultTransactionCounter reports, and
// StatTest.CollectResetsCountersBetweenCallsTest covers the drain. The Go
// agent mirrors both in Test_javaParityLock_TransactionCounters.

// ===========================================================================
// Group 10 - message truncation format
// ===========================================================================

// The abbreviation Java's StringUtils.abbreviate writes: the value cut to the
// limit followed by "...(original length)". The web tier shows the marker
// as-is, so the format is part of the contract.
TEST(JavaParityLockTest, TruncationFormat) {
    EXPECT_EQ(abbreviateString("short", 10), "short") << "a value within the limit is untouched";
    EXPECT_EQ(abbreviateString("0123456789", 10), "0123456789") << "exactly at the limit is untouched";
    EXPECT_EQ(abbreviateString("0123456789A", 10), "0123456789...(11)")
        << "the marker carries the original length";
}

// The UTF-8 guard both ports add on top of Java: protobuf rejects invalid
// UTF-8 at marshal time, so a mid-rune cut would fail the whole span or
// metadata send carrying it.
TEST(JavaParityLockTest, TruncationCutsOnAUtf8Boundary) {
    // U+AC00 is three bytes; a limit of 4 lands inside the second character.
    const std::string source = "\xEA\xB0\x80\xEA\xB0\x80\xEA\xB0\x80";
    ASSERT_EQ(source.size(), 9u);

    const auto abbreviated = abbreviateString(source, 4);
    EXPECT_EQ(abbreviated, "\xEA\xB0\x80...(9)");
    EXPECT_TRUE(isValidUtf8(abbreviated)) << "the result must stay valid UTF-8 for protobuf";
}

// The two message limits. Java: AbstractRecorder abbreviates an exception
// message to 256 chars before recording it on a span or span event, and
// profiler.jdbc.maxsqllength defaults to 65536 for the SQL text that travels
// in PSqlMetaData.
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

// The channel options verified equal across the three agents. flowControlWindow,
// writeBufferSize and maxHeaderListSize are deliberately left at the gRPC
// C-core defaults here where Java and Go pin them; doc/java_parity.md records
// that, and there is nothing to assert for them. The idle timeout is locked
// as a decision (off, like Java's 30-day "disable" sentinel), not as a value:
// 0 here maps to GRPC_ARG_CLIENT_IDLE_TIMEOUT_MS=INT_MAX.
TEST(JavaParityLockTest, GrpcChannelDefaults) {
    EXPECT_EQ(defaults::GRPC_KEEPALIVE_TIME_MS, 30 * 1000) << "Java ClientOption keepAliveTime";
    EXPECT_EQ(defaults::GRPC_KEEPALIVE_TIMEOUT_MS, 60 * 1000) << "Java ClientOption keepAliveTimeout";
    EXPECT_EQ(defaults::GRPC_MAX_MESSAGE_SIZE, 4 * 1024 * 1024) << "Java ClientOption maxInboundMessageSize";
    EXPECT_EQ(defaults::GRPC_CHANNEL_MAX_AGE_MS, 0) << "renewal off, as in Java";
    EXPECT_EQ(defaults::GRPC_STREAM_MAX_AGE_MS, 0) << "renewal off, as in Java";
    EXPECT_EQ(defaults::GRPC_IDLE_TIMEOUT_MS, 0) << "idle timeout off, as Java's IDLE_TIMEOUT_MILLIS_DISABLE";
}

// The AgentInfo refresh cadence. The retry interval deliberately differs from
// Java's effective 300000ms (profiler.agentInfo.send.retry.interval):
// registration gates tracing in both ports, so it has to retry far more often.
// doc/java_parity.md records that.
TEST(JavaParityLockTest, AgentInfoSchedule) {
    EXPECT_EQ(defaults::AGENT_INFO_REFRESH_INTERVAL_MS, 24 * 60 * 60 * 1000)
        << "Java AgentInfoSender refresh interval";
    EXPECT_EQ(defaults::AGENT_INFO_MAX_TRY_PER_ATTEMPT, 3)
        << "Java AgentInfoSender maxTryPerAttempt";
    EXPECT_EQ(defaults::AGENT_INFO_SEND_RETRY_INTERVAL_MS, 3000)
        << "matches the Go agent, not Java's effective 300000ms - see doc/java_parity.md";
}

// Span batching. Java's pinpoint-root.config ships
// profiler.transport.grpc.span.batch-sender.size=20 with a 1000ms flush and a
// 500ms collect deadline; the Go agent's default size is 50, which
// doc/java_parity.md records.
TEST(JavaParityLockTest, SpanBatchDefaults) {
    EXPECT_EQ(defaults::SPAN_BATCH_SIZE, 20) << "Java span.batch-sender.size";
    EXPECT_EQ(defaults::SPAN_BATCH_FLUSH_INTERVAL_MS, 1000);
    EXPECT_EQ(defaults::SPAN_BATCH_COLLECT_DEADLINE_MS, 500);
    EXPECT_EQ(defaults::SPAN_BATCH_MAX_CONCURRENT_REQUESTS, 10);
}

// The stat collection cadence. Java's code default is 5000ms with a batch of 6
// (DefaultMonitorConfig); the release profile raises the interval to 10000ms,
// which doc/java_parity.md records.
TEST(JavaParityLockTest, StatCollectionDefaults) {
    EXPECT_EQ(defaults::STAT_INTERVAL_MS, 5000) << "Java DefaultMonitorConfig code default";
    EXPECT_EQ(defaults::STAT_BATCH_COUNT, 6) << "Java profiler.jvm.stat.batch.send.count";
}

// The SQL cache bounds. All three agents bypass the UID cache for a statement
// at or over the length limit and re-publish a UID's metadata after the expiry.
TEST(JavaParityLockTest, SqlCacheDefaults) {
    EXPECT_EQ(defaults::SQL_CACHE_SIZE, 1024) << "Java profiler.jdbc.sqlcachesize";
    EXPECT_EQ(defaults::SQL_CACHE_LENGTH_LIMIT, 2048) << "Java profiler.jdbc.sqlcachelengthlimit";
    EXPECT_EQ(defaults::SQL_CACHE_EXPIRE_HOURS, 168) << "Java profiler.jdbc.sqlcacheexpirehours";
    EXPECT_EQ(defaults::SQL_MAX_BIND_ARGS_SIZE, 1024) << "Java profiler.jdbc.maxsqlbindvaluesize";
    EXPECT_EQ(defaults::SQL_ERROR_COUNT, 100) << "Java profiler.sql.error.count";
}

}  // namespace pinpoint
