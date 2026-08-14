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
#include <gmock/gmock.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <memory>
#include <string>
#include <map>

#include "../src/agent.h"
#include "../src/grpc.h"
#include "../src/config.h"
#include "../src/noop.h"
#include "../src/span.h"
#include "../src/stat.h"
#include "../src/url_stat.h"
#include "../src/logging.h"
#include "../include/pinpoint/tracer.h"
#include "mock_helpers.h"
#include "testable_grpc.h"

namespace pinpoint {

// --- Helper to build a valid Config ---

static std::shared_ptr<Config> make_test_config() {
    auto cfg = std::make_shared<Config>();
    cfg->enable = true;
    cfg->app_name_ = "test-app";
    cfg->agent_id_ = "test-agent-id";
    cfg->agent_name_ = "test-agent-name";
    cfg->collector.host = "127.0.0.1";
    cfg->collector.agent_port = 9991;
    cfg->collector.span_port = 9993;
    cfg->collector.stat_port = 9992;
    cfg->span.queue_size = 1024;
    cfg->span.event_chunk_size = 10;
    cfg->span.max_event_depth = 32;
    cfg->stat.enable = true;
    cfg->stat.collect_interval = 5000;
    cfg->http.url_stat.enable = true;
    cfg->http.url_stat.limit = 1024;
    cfg->http.url_stat.trim_path_depth = 3;
    cfg->sampling.type = "counter";
    cfg->sampling.counter_rate = 1;
    cfg->sql.enable_raw_sql_cache = true;
    return cfg;
}

// --- Helper to create AgentImpl with mock gRPC clients ---

static std::shared_ptr<AgentImpl> make_test_agent(std::shared_ptr<Config> cfg,
                                                  size_t cache_size = AgentImpl::kDefaultCacheSize,
                                                  const AgentOptions& options = {}) {
    auto clients = make_testable_grpc_clients(cfg);

    // createShared mirrors production (StartAgent): the SharedDeleter keeps
    // the final release bounded when a test drops the last reference
    // without calling Shutdown().
    auto agent = AgentImpl::createShared(
        cfg,
        std::move(clients.agent),
        std::move(clients.metadata),
        std::move(clients.span),
        std::move(clients.stats),
        nullptr,
        DEFAULT_APP_TYPE,
        cache_size);
    // Construction is cold; bring the agent online so the worker threads run
    // (mirrors the production StartAgent() sequence). Options are only needed
    // by tests that exercise the config-file watcher.
    agent->setOptions(options);
    agent->Start();
    return agent;
}

// --- Test fixture ---

class AgentImplTest : public ::testing::Test {
protected:
    void SetUp() override {
        cfg_ = make_test_config();
        agent_ = make_test_agent(cfg_);
        // Wait for the initial AgentInfo send to succeed and enable the agent.
        wait_until_enabled();
    }

    void TearDown() override {
        if (agent_) {
            agent_->Shutdown();
            agent_.reset();
        }
    }

    void wait_until_enabled(int timeout_ms = 3000) {
        wait_agent_enabled(agent_, timeout_ms);
    }

    static void wait_agent_enabled(const std::shared_ptr<AgentImpl>& agent, int timeout_ms = 3000) {
        wait_for_condition([&] { return agent->Enable(); }, std::chrono::milliseconds(timeout_ms));
    }

    std::shared_ptr<Config> cfg_;
    std::shared_ptr<AgentImpl> agent_;
};

// --- Tests ---

TEST_F(AgentImplTest, EnableAfterInit) {
    EXPECT_TRUE(agent_->Enable());
}

// Start() reports whether initialization is running: true on the first call
// and on idempotent repeats, false once the agent was shut down. StartAgent()
// relies on this to publish only successfully launched agents as the global
// instance — a synchronous launch failure must never install a permanently
// cold agent that later StartAgent() calls would keep returning.
TEST(AgentStartResultTest, StartReportsLaunchRepeatAndShutdownRefusal) {
    auto cfg = make_test_config();
    auto clients = make_testable_grpc_clients(cfg);
    // createShared, never make_shared: production (StartAgent) attaches the
    // SharedDeleter to every shared-owned agent, and the deadline-expiry
    // deferred-destroy path assumes it runs at final release.
    auto agent = AgentImpl::createShared(
        cfg, std::move(clients.agent), std::move(clients.metadata),
        std::move(clients.span), std::move(clients.stats));

    EXPECT_TRUE(agent->Start()) << "first Start() must report a successful launch";
    EXPECT_TRUE(agent->Start()) << "a repeated Start() is a successful no-op";

    agent->Shutdown();
    EXPECT_FALSE(agent->Start()) << "a shut-down agent must refuse Start()";
}

// An unsampled span reaches everything it records into (AgentStats, the
// url-stat sink) through its runtime snapshot, NOT through an agent
// keep-alive: it must not pin the agent — the per-span selfRef() it used to
// take was a CAS on the agent's one control block, measured as half the
// four-thread cost of the unsampled path — and, the flip side of that coin,
// ending the span after the agent is destroyed must stay safe because the
// snapshot owns the sinks independently.
TEST(AgentLifetimeTest, UnsampledSpanDoesNotPinAgentAndOutlivesItSafely) {
    auto cfg = make_test_config();
    cfg->sampling.counter_rate = 0;  // never sample → NewSpan yields UnsampledSpan
    auto agent = make_test_agent(cfg);
    ASSERT_TRUE(wait_for_condition([&] { return agent->Enable(); }, std::chrono::seconds(3)));

    auto span = agent->NewSpan("lifetime-op", "/lifetime");
    ASSERT_FALSE(span->IsSampled());
    // A real UnsampledSpan, not the noop singleton (which has span id 0).
    ASSERT_NE(span->GetSpanId(), 0);
    span->SetUrlStat("/lifetime", "GET", 200);

    std::weak_ptr<AgentImpl> observer = agent;
    agent->Shutdown();
    agent.reset();
    EXPECT_TRUE(observer.expired())
        << "an unsampled span must not keep the agent alive";

    // Records into the runtime-owned sinks (the url stat is dropped by the
    // sink's shutdown gate); under ASan/TSan this is the use-after-free probe.
    span->EndSpan();
    span.reset();
}

// Start() blocks signals only while it spawns the agent threads (so they
// inherit a blocked mask); the calling thread's own mask must be restored
// before Start() returns — the host's signal handling must not be altered.
TEST(AgentSignalMaskTest, StartRestoresCallerSignalMask) {
    sigset_t before;
    ASSERT_EQ(pthread_sigmask(SIG_BLOCK, nullptr, &before), 0);

    auto cfg = make_test_config();
    auto agent = make_test_agent(cfg);

    sigset_t after;
    ASSERT_EQ(pthread_sigmask(SIG_BLOCK, nullptr, &after), 0);
    for (const int sig : {SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2}) {
        EXPECT_EQ(sigismember(&before, sig), sigismember(&after, sig))
            << "signal " << sig << " mask changed across Start()";
    }

    agent->Shutdown();
}

TEST_F(AgentImplTest, GetConfigReturnsCorrectValues) {
    EXPECT_EQ(agent_->getAppName(), "test-app");
    EXPECT_EQ(agent_->getAppType(), 1300);
    EXPECT_EQ(agent_->getAgentId(), "test-agent-id");
    EXPECT_EQ(agent_->getConfig()->agent_name_, "test-agent-name");
    EXPECT_NE(agent_->getStartTime(), 0);
}

TEST_F(AgentImplTest, GetConfigReturnsSharedPtr) {
    auto config = agent_->getConfig();
    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config->app_name_, "test-app");
}

TEST_F(AgentImplTest, GenerateTraceIdIncrementsSequence) {
    auto tid1 = agent_->generateTraceId();
    auto tid2 = agent_->generateTraceId();

    EXPECT_EQ(tid1.agentId(), "test-agent-id");
    EXPECT_EQ(tid2.agentId(), "test-agent-id");
    EXPECT_EQ(tid1.StartTime, tid2.StartTime);
    EXPECT_LT(tid1.Sequence, tid2.Sequence);
}

TEST_F(AgentImplTest, NewSpanReturnsValidSpan) {
    auto span = agent_->NewSpan("test-op", "/test/rpc");
    ASSERT_NE(span, nullptr);
}

TEST_F(AgentImplTest, NewSpanWithReaderReturnsValidSpan) {
    NoopTraceContextReader reader;
    auto span = agent_->NewSpan("test-op", "/test/rpc", reader);
    ASSERT_NE(span, nullptr);
}

TEST_F(AgentImplTest, NewSpanWithMethodReturnsValidSpan) {
    NoopTraceContextReader reader;
    auto span = agent_->NewSpan("test-op", "/test/rpc", "GET", reader);
    ASSERT_NE(span, nullptr);
}

TEST_F(AgentImplTest, NewSpanWithValidTraceIdHeaderIsSampled) {
    // counter_rate=1 → the request is sampled; a well-formed inbound trace id
    // parses, so the continued trace is recorded as a real (sampled) span.
    MockTraceContextReader reader;
    reader.SetContext(HEADER_TRACE_ID, "upstream-agent^1700000000^7");
    auto span = agent_->NewSpan("test-op", "/test/rpc", reader);
    ASSERT_NE(span, nullptr);
    EXPECT_TRUE(span->IsSampled()) << "a valid continued trace should be sampled";
}

TEST_F(AgentImplTest, NewSpanWithMalformedTraceIdHeaderReturnsNoop) {
    // Same sampling decision, but the malformed inbound trace id fails to parse.
    // NewSpan must fall back to a non-sampled noop span instead of recording a
    // trace with no agent id.
    MockTraceContextReader reader;
    reader.SetContext(HEADER_TRACE_ID, "this-is-not-a-valid-trace-id");
    auto span = agent_->NewSpan("test-op", "/test/rpc", reader);
    ASSERT_NE(span, nullptr);
    EXPECT_FALSE(span->IsSampled()) << "a malformed inbound trace id should yield a noop span";
}

TEST_F(AgentImplTest, RecordSpanDoesNotCrash) {
    auto span_data = make_test_span_data_ptr(*agent_, "test-op");
    auto span_chunk = std::make_unique<SpanChunk>(span_data, true);
    agent_->recordSpan(std::move(span_chunk));
}

TEST_F(AgentImplTest, RecordStatsDoesNotCrash) {
    agent_->recordStats(AGENT_STATS);
    agent_->recordStats(URL_STATS);
}

TEST_F(AgentImplTest, CacheApiReturnsNonZeroId) {
    int32_t id = agent_->cacheApi("com.example.Api", 100);
    EXPECT_NE(id, 0);
}

TEST_F(AgentImplTest, CacheApiReturnsSameIdForSameKey) {
    int32_t id1 = agent_->cacheApi("com.example.Api", 100);
    int32_t id2 = agent_->cacheApi("com.example.Api", 100);
    EXPECT_EQ(id1, id2);
}

TEST_F(AgentImplTest, CacheApiReturnsDifferentIdForDifferentKeys) {
    int32_t id1 = agent_->cacheApi("com.example.Api1", 100);
    int32_t id2 = agent_->cacheApi("com.example.Api2", 100);
    EXPECT_NE(id1, id2);
}

TEST_F(AgentImplTest, CacheApiReturnsDifferentIdForDifferentTypes) {
    int32_t id1 = agent_->cacheApi("com.example.Api", 100);
    int32_t id2 = agent_->cacheApi("com.example.Api", 200);
    EXPECT_NE(id1, id2);
}

TEST_F(AgentImplTest, CacheErrorReturnsNonZeroId) {
    int32_t id = agent_->cacheError("TestError");
    EXPECT_NE(id, 0);
}

TEST_F(AgentImplTest, CacheErrorReturnsSameIdForSameKey) {
    int32_t id1 = agent_->cacheError("TestError");
    int32_t id2 = agent_->cacheError("TestError");
    EXPECT_EQ(id1, id2);
}

TEST_F(AgentImplTest, CacheSqlReturnsNonZeroId) {
    int32_t id = agent_->cacheSql("SELECT * FROM test");
    EXPECT_NE(id, 0);
}

TEST_F(AgentImplTest, CacheSqlReturnsSameIdForSameQuery) {
    int32_t id1 = agent_->cacheSql("SELECT 1");
    int32_t id2 = agent_->cacheSql("SELECT 1");
    EXPECT_EQ(id1, id2);
}

TEST_F(AgentImplTest, PrepareSqlCachesCompleteRawResult) {
    constexpr std::string_view raw_sql =
        "SELECT * FROM users WHERE id = 42 AND state = 'READY'";

    auto first = agent_->prepareSql(raw_sql, SqlMetaMode::Id);
    auto second = agent_->prepareSql(raw_sql, SqlMetaMode::Id);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_NE(*first, nullptr);
    EXPECT_EQ(*first, *second);
    EXPECT_EQ((*first)->parameters, "42,READY");
    EXPECT_GT(std::get<int32_t>((*first)->identity), 0);
}

TEST_F(AgentImplTest, PrepareSqlSkipsRawCacheWhenDisabled) {
    auto config = std::make_shared<Config>(*agent_->getConfig());
    config->sql.enable_raw_sql_cache = false;
    agent_->reloadConfig(config);

    constexpr std::string_view raw_sql =
        "SELECT * FROM users WHERE id = 42 AND state = 'READY'";
    auto first = agent_->prepareSql(raw_sql, SqlMetaMode::Id);
    auto second = agent_->prepareSql(raw_sql, SqlMetaMode::Id);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_NE(*first, nullptr);
    ASSERT_NE(*second, nullptr);
    EXPECT_NE(*first, *second);
    EXPECT_EQ((*first)->parameters, (*second)->parameters);
    EXPECT_EQ(std::get<int32_t>((*first)->identity),
              std::get<int32_t>((*second)->identity));
}

TEST_F(AgentImplTest, PrepareSqlRawVariantsShareCanonicalIdentity) {
    auto first = agent_->prepareSql(
        "SELECT * FROM users WHERE id = 41", SqlMetaMode::Id);
    auto second = agent_->prepareSql(
        "SELECT * FROM users WHERE id = 42", SqlMetaMode::Id);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(*first, *second);
    EXPECT_EQ(std::get<int32_t>((*first)->identity),
              std::get<int32_t>((*second)->identity));
    EXPECT_EQ((*first)->parameters, "41");
    EXPECT_EQ((*second)->parameters, "42");
}

TEST_F(AgentImplTest, PrepareSqlMetadataFailureEpochInvalidatesRawEntry) {
    constexpr std::string_view raw_sql = "SELECT * FROM users WHERE id = 42";
    auto first = agent_->prepareSql(raw_sql, SqlMetaMode::Id);
    ASSERT_TRUE(first.has_value());
    ASSERT_NE(*first, nullptr);
    const auto first_id = std::get<int32_t>((*first)->identity);

    agent_->removeCacheSql(StringMeta{
        first_id, "SELECT * FROM users WHERE id = 0#", STRING_META_SQL});
    auto reloaded = agent_->prepareSql(raw_sql, SqlMetaMode::Id);

    ASSERT_TRUE(reloaded.has_value());
    ASSERT_NE(*reloaded, nullptr);
    EXPECT_NE(*first, *reloaded);
    EXPECT_NE(first_id, std::get<int32_t>((*reloaded)->identity));
    EXPECT_EQ((*first)->parameters, (*reloaded)->parameters);
}

TEST_F(AgentImplTest, PrepareSqlKeepsIdAndUidNamespacesIndependent) {
    constexpr std::string_view raw_sql = "SELECT * FROM users WHERE id = 42";
    auto id_entry = agent_->prepareSql(raw_sql, SqlMetaMode::Id);
    auto uid_entry = agent_->prepareSql(raw_sql, SqlMetaMode::Uid);

    ASSERT_TRUE(id_entry.has_value());
    ASSERT_TRUE(uid_entry.has_value());
    EXPECT_NE(*id_entry, *uid_entry);
    EXPECT_TRUE(std::holds_alternative<int32_t>((*id_entry)->identity));
    EXPECT_TRUE(std::holds_alternative<SqlUid>((*uid_entry)->identity));

    const auto uid = std::get<SqlUid>((*uid_entry)->identity);
    agent_->removeCacheSqlUid(SqlUidMeta{uid, "SELECT * FROM users WHERE id = 0#"});
    auto reloaded_uid = agent_->prepareSql(raw_sql, SqlMetaMode::Uid);

    ASSERT_TRUE(reloaded_uid.has_value());
    EXPECT_NE(*uid_entry, *reloaded_uid);
    EXPECT_EQ(uid, std::get<SqlUid>((*reloaded_uid)->identity));
    auto id_hit = agent_->prepareSql(raw_sql, SqlMetaMode::Id);
    ASSERT_TRUE(id_hit.has_value());
    EXPECT_EQ(*id_entry, *id_hit);
}

TEST_F(AgentImplTest, PrepareSqlReturnsNulloptWhenAgentDisabled) {
    // Shutdown() clears enabled_; prepareSql must then short-circuit to nullopt
    // for both namespaces instead of normalizing or touching the caches.
    agent_->Shutdown();
    ASSERT_FALSE(agent_->Enable());

    EXPECT_FALSE(
        agent_->prepareSql("SELECT * FROM users WHERE id = 42", SqlMetaMode::Id)
            .has_value());
    EXPECT_FALSE(
        agent_->prepareSql("SELECT * FROM users WHERE id = 42", SqlMetaMode::Uid)
            .has_value());
}

TEST_F(AgentImplTest, CacheSqlUidReturnsNonEmpty) {
    auto uid = agent_->cacheSqlUid("SELECT 1");
    ASSERT_TRUE(uid.has_value());
    EXPECT_EQ(uid->size(), 16u);
}

// cacheSqlUid returns std::optional and yields nullopt when the agent is disabled
// (guarded before touching the cache). SpanEvent recording gates the SQL-UID
// annotation on this optional, so a disabled agent must not hand out a UID that
// would produce an annotation referencing metadata that was never sent.
TEST_F(AgentImplTest, CacheSqlUidReturnsNulloptWhenDisabled) {
    ASSERT_TRUE(agent_->cacheSqlUid("SELECT 1").has_value()) << "enabled agent returns a UID";

    agent_->Shutdown();
    ASSERT_FALSE(agent_->Enable());
    EXPECT_FALSE(agent_->cacheSqlUid("SELECT 1").has_value())
        << "a disabled agent must return nullopt, not a UID";
}

TEST_F(AgentImplTest, ShutdownDisablesAgent) {
    EXPECT_TRUE(agent_->Enable());
    agent_->Shutdown();
    EXPECT_FALSE(agent_->Enable());
}

TEST_F(AgentImplTest, ShutdownIsIdempotent) {
    agent_->Shutdown();
    agent_->Shutdown();
    EXPECT_FALSE(agent_->Enable());
}

TEST_F(AgentImplTest, NewSpanAfterShutdownReturnsNoop) {
    agent_->Shutdown();
    auto span = agent_->NewSpan("test-op", "/test/rpc");
    ASSERT_NE(span, nullptr);
    // After shutdown, span should be a noop — calling methods should not crash
    span->SetEndPoint("ep");
    span->SetRemoteAddress("1.2.3.4");
    span->EndSpan();
}

TEST_F(AgentImplTest, CacheApiAfterShutdownReturnsZero) {
    agent_->Shutdown();
    int32_t id = agent_->cacheApi("com.example.Api", 100);
    EXPECT_EQ(id, 0);
}

TEST_F(AgentImplTest, CacheErrorAfterShutdownReturnsZero) {
    agent_->Shutdown();
    int32_t id = agent_->cacheError("TestError");
    EXPECT_EQ(id, 0);
}

TEST_F(AgentImplTest, CacheSqlAfterShutdownReturnsZero) {
    agent_->Shutdown();
    int32_t id = agent_->cacheSql("SELECT 1");
    EXPECT_EQ(id, 0);
}

TEST_F(AgentImplTest, RecordSpanAfterShutdownDoesNotCrash) {
    agent_->Shutdown();
    auto span_data = make_test_span_data_ptr(*agent_, "test-op");
    auto span_chunk = std::make_unique<SpanChunk>(span_data, true);
    agent_->recordSpan(std::move(span_chunk));
}

TEST_F(AgentImplTest, RecordStatsAfterShutdownDoesNotCrash) {
    agent_->Shutdown();
    agent_->recordStats(AGENT_STATS);
}

TEST_F(AgentImplTest, IsExitingReflectsShutdown) {
    EXPECT_FALSE(agent_->isExiting());
    agent_->Shutdown();
    EXPECT_TRUE(agent_->isExiting());
}

// --- Terminal-shutdown contract ---
//
// Shutdown() cannot be undone: shutting_down_ is never cleared, so Start()
// refuses for the rest of the process. Hosts that stop and resume tracing must
// build a fresh agent instead of restarting this one (see Agent::Start() /
// Agent::Shutdown() in pinpoint/tracer.h). These pin that contract so a future
// change cannot silently turn a rejected restart into a half-started agent.

TEST_F(AgentImplTest, StartAfterShutdownIsRefused) {
    agent_->Shutdown();
    ASSERT_TRUE(agent_->isExiting());

    EXPECT_NO_THROW(agent_->Start());

    // Give a wrongly-accepted Start() the same window the fixture allows a
    // real one, so the assertion below cannot pass merely by being too early.
    wait_until_enabled(500);
    EXPECT_FALSE(agent_->Enable())
        << "a shut-down agent must stay offline; it cannot be restarted";
    EXPECT_TRUE(agent_->isExiting());
}

TEST_F(AgentImplTest, StartAfterShutdownKeepsServingNoopSpans) {
    agent_->Shutdown();
    agent_->Start();
    wait_until_enabled(500);

    // The application keeps working against the dead agent: every span is the
    // shared noop singleton and recording through it is inert, not a crash.
    for (int i = 0; i < 3; ++i) {
        auto span = agent_->NewSpan("post-restart", "/post-restart");
        ASSERT_NE(span, nullptr);
        EXPECT_EQ(span, noopSpan());
        EXPECT_FALSE(span->IsSampled());
        span->SetStatusCode(200);
        auto* event = span->NewSpanEvent("work");
        ASSERT_NE(event, nullptr);
        event->EndEvent();
        span->EndSpan();
    }
    EXPECT_EQ(agent_->cacheApi("com.example.Api", 100), 0);
}

TEST_F(AgentImplTest, RepeatedShutdownStartSequencesStayRefused) {
    // A host looping "stop then start" must not accumulate half-started state
    // (re-spawned workers over live handles would terminate the process).
    for (int cycle = 0; cycle < 3; ++cycle) {
        EXPECT_NO_THROW({
            agent_->Shutdown();
            agent_->Start();
        }) << "cycle " << cycle;
        EXPECT_FALSE(agent_->Enable()) << "cycle " << cycle;
    }
}

// --- Destructor safety tests ---
//
// These guard against the SIGABRT-on-exit reported when the host process
// destroys the agent without first calling Shutdown(): the dtor used to
// let exceptions escape (terminate()) and re-enter global_agent.reset()
// while it was itself being destroyed.

TEST_F(AgentImplTest, DtorAfterImplicitShutdownDoesNotThrow) {
    // The fixture's TearDown calls Shutdown(); take a separately-scoped
    // agent that is deliberately destroyed without an explicit Shutdown.
    EXPECT_NO_THROW({
        auto agent = make_test_agent(make_test_config());
        wait_agent_enabled(agent);
        // Deliberately do NOT call Shutdown() — dtor must clean up safely.
    });
}

TEST_F(AgentImplTest, DtorAfterExplicitShutdownDoesNotThrow) {
    EXPECT_NO_THROW({
        auto agent = make_test_agent(make_test_config());
        wait_agent_enabled(agent);
        agent->Shutdown();
        agent.reset();  // dtor — must not throw
    });
}

TEST_F(AgentImplTest, DoubleShutdownIsNoOp) {
    auto agent = make_test_agent(make_test_config());
    wait_agent_enabled(agent);
    EXPECT_NO_THROW({
        agent->Shutdown();
        agent->Shutdown();  // must not throw, must not deadlock
    });
    EXPECT_FALSE(agent->Enable());
}

TEST_F(AgentImplTest, DtorBeforeInitCompletesDoesNotThrow) {
    // Construct and immediately destroy without waiting for init_thread_
    // to finish — exercises the "enabled_ was never set" cleanup path
    // that the old dtor's `if (enabled_) Shutdown()` guard skipped.
    EXPECT_NO_THROW({
        auto agent = make_test_agent(make_test_config());
        // No wait_agent_enabled — destroy while init may still be racing.
    });
}

// --- Shutdown deadline ---
//
// Stop signals are best-effort against a wedged RPC (gRPC TryCancel gives no
// completion bound), so the blocking teardown runs under a hard deadline:
// Shutdown() must return once it expires, handing the joins to a background
// reaper that keeps the agent alive until the straggler actually finishes.

namespace {

// A GrpcAgent whose registration wedges the init thread until released,
// ignoring every stop signal — the mock equivalent of an RPC that never
// honors cancellation. Its destruction is observable through a flag that
// outlives the agent, so tests can watch a deferred destroy complete.
class WedgedRegisterGrpcAgent : public TestableGrpcAgent {
public:
    explicit WedgedRegisterGrpcAgent(std::shared_ptr<const Config> config)
        : TestableGrpcAgent(std::move(config)) {}

    ~WedgedRegisterGrpcAgent() override { destroyed_->store(true); }

    GrpcRequestStatus registerAgent() override {
        std::unique_lock<std::mutex> lock(m_);
        wedged_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] { return released_; });
        return SEND_FAIL;
    }

    void waitUntilWedged() {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [this] { return wedged_; });
    }

    void release() {
        // Keep the condition variable protected until notification returns.
        // Once the waiter observes released_, the teardown reaper may join it
        // and destroy this mock immediately; notifying after unlocking would
        // therefore race with condition_variable destruction under TSan.
        std::lock_guard<std::mutex> lock(m_);
        released_ = true;
        cv_.notify_all();
    }

    std::shared_ptr<std::atomic<bool>> destroyedFlag() const { return destroyed_; }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool wedged_{false};
    bool released_{false};
    std::shared_ptr<std::atomic<bool>> destroyed_ =
        std::make_shared<std::atomic<bool>>(false);
};

// Builds a shared agent (production factory) whose init thread wedges in
// registration; returns the agent and the wedged client (owned by the agent).
std::shared_ptr<AgentImpl> make_wedged_agent(const std::shared_ptr<Config>& cfg,
                                             WedgedRegisterGrpcAgent** wedged_out) {
    auto wedged_owner = std::make_unique<WedgedRegisterGrpcAgent>(cfg);
    *wedged_out = wedged_owner.get();
    (*wedged_out)->injectMockStubs();
    auto grpc_metadata = std::make_unique<TestableGrpcMetadata>(cfg);
    auto grpc_span = std::make_unique<TestableGrpcSpan>(cfg);
    auto grpc_stat = std::make_unique<TestableGrpcStats>(cfg);
    grpc_metadata->injectMockStubs();
    grpc_span->injectMockStubs();
    grpc_stat->injectMockStubs();

    auto agent = AgentImpl::createShared(
        cfg, std::move(wedged_owner), std::move(grpc_metadata),
        std::move(grpc_span), std::move(grpc_stat));
    agent->Start();
    (*wedged_out)->waitUntilWedged();
    return agent;
}

// Restores the production deadline no matter how the test exits.
struct ShutdownDeadlineGuard {
    explicit ShutdownDeadlineGuard(std::chrono::milliseconds deadline) {
        set_agent_shutdown_deadline(deadline);
    }
    ~ShutdownDeadlineGuard() {
        set_agent_shutdown_deadline(std::chrono::milliseconds(0));
    }
};

}  // namespace

TEST(AgentShutdownDeadlineTest, ShutdownReturnsByDeadlineWithWedgedWorker) {
    ShutdownDeadlineGuard deadline_guard(std::chrono::milliseconds(200));

    WedgedRegisterGrpcAgent* wedged = nullptr;
    auto agent = make_wedged_agent(make_test_config(), &wedged);

    const auto start = std::chrono::steady_clock::now();
    agent->Shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds(2))
        << "Shutdown() must return by the deadline even with a wedged worker";

    // The deadline reaper must keep the agent alive while the wedged worker
    // still dereferences it. `wedged` stays valid through the keep-alive:
    // the reaper cannot finish (and release the agent) before release().
    std::weak_ptr<AgentImpl> weak = agent;
    agent.reset();
    EXPECT_FALSE(weak.expired())
        << "the reaper must hold the agent alive while a worker still runs";

    // ...and release it once the straggler finishes.
    wedged->release();
    EXPECT_TRUE(wait_for_condition([&] { return weak.expired(); }, std::chrono::seconds(5)))
        << "the reaper must release the agent once the wedged worker finishes";
}

// Dropping the last reference WITHOUT Shutdown() must be just as bounded:
// the SharedDeleter runs the teardown under the deadline and, on expiry,
// defers destruction to the teardown runner instead of joining unbounded.
// The object stays leaked exactly as long as the straggler runs and is
// destroyed once it finishes.
TEST(AgentShutdownDeadlineTest, ReleaseWithoutShutdownDefersDestructionWhenWedged) {
    ShutdownDeadlineGuard deadline_guard(std::chrono::milliseconds(200));

    WedgedRegisterGrpcAgent* wedged = nullptr;
    auto agent = make_wedged_agent(make_test_config(), &wedged);
    const auto destroyed = wedged->destroyedFlag();

    const auto start = std::chrono::steady_clock::now();
    agent.reset();  // final release, deliberately without Shutdown()
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds(2))
        << "the final release must return by the deadline even with a wedged worker";

    // Destruction was deferred, not skipped-and-freed: the object (and the
    // wedged client it owns) must still be alive while the worker runs.
    EXPECT_FALSE(destroyed->load())
        << "the runner must not destroy the agent while a worker still runs";

    // Once the straggler finishes, the runner joins it and destroys the agent.
    wedged->release();
    EXPECT_TRUE(wait_for_condition([&] { return destroyed->load(); }, std::chrono::seconds(5)))
        << "the runner must destroy the agent once the wedged worker finishes";
}

// --- URL filter tests ---

TEST_F(AgentImplTest, UrlFilterExcludesMatchingUrl) {
    auto cfg = make_test_config();
    cfg->http.server.exclude_url = {"/health", "/status"};
    auto agent = make_test_agent(cfg);
    wait_agent_enabled(agent);

    auto span = agent->NewSpan("op", "/health");
    // /health is filtered — should return noop span
    // Noop span's End() should not crash
    span->EndSpan();

    agent->Shutdown();
}

TEST_F(AgentImplTest, MethodFilterExcludesMatchingMethod) {
    auto cfg = make_test_config();
    cfg->http.server.exclude_method = {"OPTIONS"};
    auto agent = make_test_agent(cfg);
    wait_agent_enabled(agent);

    NoopTraceContextReader reader;
    auto span = agent->NewSpan("op", "/api", "OPTIONS", reader);
    span->EndSpan();

    agent->Shutdown();
}

// --- Status error tests ---

TEST_F(AgentImplTest, IsStatusFailWithDefault5xx) {
    // Default config has status_errors = {"5xx"}
    EXPECT_FALSE(agent_->isStatusFail(200));
    EXPECT_FALSE(agent_->isStatusFail(404));
    EXPECT_TRUE(agent_->isStatusFail(500));
    EXPECT_TRUE(agent_->isStatusFail(503));
}

TEST_F(AgentImplTest, IsStatusFailWithConfiguredErrors) {
    auto cfg = make_test_config();
    cfg->http.server.status_errors = {"500", "503"};
    auto agent = make_test_agent(cfg);
    wait_agent_enabled(agent);

    EXPECT_TRUE(agent->isStatusFail(500));
    EXPECT_TRUE(agent->isStatusFail(503));
    EXPECT_FALSE(agent->isStatusFail(502));
    EXPECT_FALSE(agent->isStatusFail(404));

    agent->Shutdown();
}

// --- ReloadConfig tests ---

TEST_F(AgentImplTest, ReloadConfigUpdatesSampling) {
    auto new_cfg = make_test_config();
    new_cfg->sampling.counter_rate = 100;
    agent_->reloadConfig(new_cfg);

    auto config = agent_->getConfig();
    EXPECT_EQ(config->sampling.counter_rate, 100);
}

TEST_F(AgentImplTest, ReloadConfigUpdatesUrlFilter) {
    auto new_cfg = make_test_config();
    new_cfg->http.server.exclude_url = {"/excluded"};
    agent_->reloadConfig(new_cfg);

    auto span = agent_->NewSpan("op", "/excluded");
    // Should be filtered — noop span
    span->EndSpan();
}

// --- Disabled agent test (no background thread startup) ---

class AgentImplDisabledTest : public ::testing::Test {};

TEST_F(AgentImplDisabledTest, DisabledConfigReturnsNotEnabled) {
    auto cfg = make_test_config();
    cfg->sampling.counter_rate = 0;
    auto agent = make_test_agent(cfg);

    // Even with sampling rate 0, the agent itself is enabled after AgentInfo succeeds.
    wait_for_condition([&] { return agent->Enable(); }, std::chrono::seconds(3));

    // NewSpan should return unsampled span (not noop)
    auto span = agent->NewSpan("op", "/test");
    ASSERT_NE(span, nullptr);

    agent->Shutdown();
}

// --- StartAgent tests ---

class StartAgentTest : public ::testing::Test {
protected:
    // Configuration sources flow through AgentOptions now; these fixture
    // shims keep the two-step test shape (set_config_string(...) then
    // StartAgent()/make_config()) while routing through the real API.
    AgentOptions options_;
    void set_config_string(std::string_view yaml) { options_.config_yaml = yaml; }
    void set_config_file_path(std::string_view path) { options_.config_file_path = path; }
    std::shared_ptr<Config> make_config(const std::shared_ptr<const Config>& old = nullptr) {
        return pinpoint::make_config(options_, old);
    }
    bool StartAgent() { return pinpoint::StartAgent(options_); }

    void SetUp() override {
        // Ensure clean global state
        reset_global_agent();
        options_ = AgentOptions{};
        const auto* test_info = ::testing::UnitTest::GetInstance()->current_test_info();
        log_file_ = std::filesystem::temp_directory_path() /
                    (std::string("test_pinpoint_") + test_info->test_suite_name() +
                     "_" + test_info->name() + ".log");
        watcher_config_file_ = std::filesystem::temp_directory_path() /
                    (std::string("test_pinpoint_") + test_info->test_suite_name() +
                     "_" + test_info->name() + ".yaml");
        cleanup_log_file();
        cleanup_watcher_config_file();
    }

    void TearDown() override {
        // Shutdown any global agent and clean up
        auto agent = GlobalAgent();
        auto agent_impl = std::dynamic_pointer_cast<AgentImpl>(agent);
        if (agent_impl) {
            agent_impl->Shutdown();
        }
        reset_global_agent();
        options_ = AgentOptions{};
        Logger::getInstance().shutdown();
        Logger::getInstance().setLogLevel("info");
        cleanup_log_file();
        cleanup_watcher_config_file();
    }

    // Install a mock-based agent as the global agent with the given config.
    // Pass options to give the agent a config-file watcher.
    std::shared_ptr<AgentImpl> install_mock_agent(std::shared_ptr<Config> cfg,
                                                  const AgentOptions& options = {}) {
        auto agent = make_test_agent(cfg, AgentImpl::kDefaultCacheSize, options);
        set_global_agent(agent);
        wait_agent_enabled(agent);
        return agent;
    }

    // Create a config that matches make_config() YAML defaults for non-reloadable fields
    // so that stopXxxWorker() methods work correctly after reloadConfig
    static std::shared_ptr<Config> make_test_config_for_create_agent() {
        auto cfg = make_test_config();
        cfg->http.url_stat.enable = false;
        return cfg;
    }

    static void wait_agent_enabled(const std::shared_ptr<AgentImpl>& agent, int timeout_ms = 3000) {
        wait_for_condition([&] { return agent->Enable(); }, std::chrono::milliseconds(timeout_ms));
    }

    void start_log_capture() {
        Logger::getInstance().shutdown();
        cleanup_log_file();
        Logger::getInstance().setLogLevel("info");
        Logger::getInstance().setFileLogger(log_file_.string(), 10);
    }

    std::string read_captured_log() {
        Logger::getInstance().shutdown();
        std::ifstream ifs(log_file_);
        return {std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
    }

    void cleanup_log_file() {
        std::error_code ec;
        std::filesystem::remove(log_file_, ec);
        std::filesystem::remove(log_file_.string() + ".1", ec);
    }

    // enable_watcher=true opts in to the config-file watcher
    // (EnableConfigFileWatcher); false omits the key entirely, exercising
    // the default-off behavior.
    void write_watcher_config(int counter_rate, std::string_view application_name,
                              bool exclude_test_url, bool enable_watcher = true) const {
        std::ofstream config_file(watcher_config_file_);
        ASSERT_TRUE(config_file.is_open());
        config_file
            << "ApplicationName: " << application_name << "\n"
            << "AgentName: test-agent-name\n"
            << "Enable: true\n";
        if (enable_watcher) {
            config_file << "EnableConfigFileWatcher: true\n";
        }
        config_file
            << "Collector:\n"
            << "  GrpcHost: 127.0.0.1\n"
            << "  GrpcAgentPort: 9991\n"
            << "  GrpcSpanPort: 9993\n"
            << "  GrpcStatPort: 9992\n"
            << "Sampling:\n"
            << "  Type: COUNTER\n"
            << "  CounterRate: " << counter_rate << "\n"
            << "Http:\n"
            << "  Server:\n"
            << "    ExcludeUrl: "
            << (exclude_test_url ? "[/watcher-excluded]" : "[]") << "\n";
        ASSERT_TRUE(config_file.good());
    }

    void cleanup_watcher_config_file() {
        std::error_code ec;
        std::filesystem::remove(watcher_config_file_, ec);
    }

    std::filesystem::path log_file_;
    std::filesystem::path watcher_config_file_;

    static constexpr const char* kBaseConfigYaml = R"(
ApplicationName: test-app
AgentName: test-agent-name
Enable: true
Collector:
  GrpcHost: 127.0.0.1
  GrpcAgentPort: 9991
  GrpcSpanPort: 9993
  GrpcStatPort: 9992
Sampling:
  Type: COUNTER
  CounterRate: 1
)";
};

// StartAgent() is one-shot per process: a second call reports success but
// leaves the running agent untouched. Config changes flow through the
// config-file watcher (or an explicit reloadConfig()), not through repeated
// StartAgent() calls.
TEST_F(StartAgentTest, StartAgentAgainReturnsRunningAgentUnchanged) {
    // 1. Install a mock agent as the global agent
    auto cfg = make_test_config_for_create_agent();
    auto original_agent = install_mock_agent(cfg);
    ASSERT_TRUE(original_agent->Enable());
    EXPECT_EQ(original_agent->getConfig()->sampling.counter_rate, 1);

    // 2. Call StartAgent() again with a different config source
    std::string changed_config = R"(
ApplicationName: test-app
AgentName: test-agent-name
Enable: true
Collector:
  GrpcHost: 127.0.0.1
  GrpcAgentPort: 9991
  GrpcSpanPort: 9993
  GrpcStatPort: 9992
Sampling:
  Type: COUNTER
  CounterRate: 50
)";
    set_config_string(changed_config);
    EXPECT_TRUE(StartAgent()) << "a repeated StartAgent() must report success";

    // 3. Verify: the running instance stays installed, untouched
    auto installed = std::dynamic_pointer_cast<AgentImpl>(GlobalAgent());
    ASSERT_NE(installed, nullptr) << "the global agent must stay a real agent, not noop";
    EXPECT_EQ(installed.get(), original_agent.get()) << "the running agent must stay installed";
    EXPECT_EQ(installed->getConfig()->sampling.counter_rate, 1)
        << "a repeated StartAgent() must not reload the running config";
}

// reloadConfig() with a make_config(options, old) rebuild applies reloadable
// fields — the same path the config-file watcher drives.
TEST_F(StartAgentTest, ReloadConfigAppliesReloadableFields) {
    auto cfg = make_test_config_for_create_agent();
    auto agent = install_mock_agent(cfg);
    ASSERT_TRUE(agent->Enable());
    EXPECT_EQ(agent->getConfig()->sampling.counter_rate, 1);

    std::string reloadable_config = R"(
ApplicationName: test-app
AgentName: test-agent-name
Enable: true
Collector:
  GrpcHost: 127.0.0.1
  GrpcAgentPort: 9991
  GrpcSpanPort: 9993
  GrpcStatPort: 9992
Sampling:
  Type: COUNTER
  CounterRate: 50
)";
    set_config_string(reloadable_config);
    auto reload_cfg = make_config(agent->getConfig());
    ASSERT_NE(reload_cfg, nullptr);
    agent->reloadConfig(reload_cfg);

    EXPECT_EQ(agent->getConfig()->sampling.counter_rate, 50);
}

TEST_F(StartAgentTest, ConfigFileWatcherReloadsChangesAndStopsPromptly) {
    write_watcher_config(1, "test-app", false);
    set_config_file_path(watcher_config_file_.string());

    // Shrink the watcher's poll tick (production default: 1s) so the reload
    // below is picked up in tens of milliseconds. Also verifies the injected
    // interval is honored: with the 1s default the shortened waits in this
    // test would never observe a reload. Reset in ScopedPollInterval so later
    // watcher starts see the production default again.
    struct ScopedPollInterval {
        ~ScopedPollInterval() { set_config_watcher_poll_interval(std::chrono::milliseconds(0)); }
    } reset_poll_interval;
    set_config_watcher_poll_interval(std::chrono::milliseconds(20));

    auto initial_config = make_config();
    ASSERT_NE(initial_config, nullptr);
    // The options carry the config file path, so the agent's Start() installs
    // its own watcher on that file.
    auto agent = install_mock_agent(initial_config, options_);
    ASSERT_TRUE(agent->Enable());
    ASSERT_EQ(agent->getConfig()->sampling.counter_rate, 1);

    // Let the watcher seed its initial mtime and complete one polling tick.
    // The later explicit mtime change keeps this deterministic even on file
    // systems with coarse timestamp resolution.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::error_code ec;
    const auto previous_mtime =
        std::filesystem::last_write_time(watcher_config_file_, ec);
    ASSERT_FALSE(ec);

    write_watcher_config(50, "attempted-app-name-change", true);
    std::filesystem::last_write_time(
        watcher_config_file_, previous_mtime + std::chrono::seconds(5), ec);
    ASSERT_FALSE(ec);

    wait_for_condition([&] { return agent->getConfig()->sampling.counter_rate == 50; },
                       std::chrono::seconds(4));

    const auto reloaded = agent->getConfig();
    ASSERT_EQ(reloaded->sampling.counter_rate, 50);
    EXPECT_EQ(reloaded->app_name_, "test-app")
        << "non-reloadable identity must be retained";
    auto excluded = agent->NewSpan("watcher.test", "/watcher-excluded");
    ASSERT_NE(excluded, nullptr);
    EXPECT_FALSE(excluded->IsSampled());
    excluded->EndSpan();

    // Stop must return promptly: Shutdown() joins the agent-owned watcher
    // first, then the mocked gRPC workers (which finish immediately). The
    // strict "stop wakes a long tick wait" regression coverage lives in
    // ConfigFileWatcherStopWakesLongPollTick, which injects a tick far longer
    // than this bound.
    const auto stop_started = std::chrono::steady_clock::now();
    agent->Shutdown();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;
    EXPECT_LT(stop_elapsed, std::chrono::milliseconds(2000));

    EXPECT_FALSE(agent->Enable());
}

// EnableConfigFileWatcher defaults to false: without opting in, an agent
// started with a config file path must NOT install the watcher, so editing
// the file at runtime leaves the running config untouched.
TEST_F(StartAgentTest, ConfigFileWatcherNotInstalledByDefault) {
    write_watcher_config(1, "test-app", false, /*enable_watcher=*/false);
    set_config_file_path(watcher_config_file_.string());

    // A short tick makes the negative check meaningful: with the watcher
    // (wrongly) installed, many polls fit into the wait below and the reload
    // would be observed.
    struct ScopedPollInterval {
        ~ScopedPollInterval() { set_config_watcher_poll_interval(std::chrono::milliseconds(0)); }
    } reset_poll_interval;
    set_config_watcher_poll_interval(std::chrono::milliseconds(20));

    auto initial_config = make_config();
    ASSERT_NE(initial_config, nullptr);
    EXPECT_FALSE(initial_config->enable_config_file_watcher);
    auto agent = install_mock_agent(initial_config, options_);
    ASSERT_TRUE(agent->Enable());
    ASSERT_EQ(agent->getConfig()->sampling.counter_rate, 1);

    std::error_code ec;
    const auto previous_mtime =
        std::filesystem::last_write_time(watcher_config_file_, ec);
    ASSERT_FALSE(ec);
    write_watcher_config(50, "test-app", false, /*enable_watcher=*/false);
    std::filesystem::last_write_time(
        watcher_config_file_, previous_mtime + std::chrono::seconds(5), ec);
    ASSERT_FALSE(ec);

    // Give a hypothetical watcher ample polls to pick the change up, then
    // verify nothing was reloaded.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(agent->getConfig()->sampling.counter_rate, 1)
        << "config must not be hot-reloaded when EnableConfigFileWatcher is off";

    agent->Shutdown();
    EXPECT_FALSE(agent->Enable());
}

TEST_F(StartAgentTest, ConfigFileWatcherStopWakesLongPollTick) {
    write_watcher_config(1, "test-app", false);

    // A 5s tick makes an un-woken wait unmissable: if the stop signal did not
    // interrupt the wait, join would block for seconds and the assertion
    // below would fail loudly.
    struct ScopedPollInterval {
        ~ScopedPollInterval() { set_config_watcher_poll_interval(std::chrono::milliseconds(0)); }
    } reset_poll_interval;
    set_config_watcher_poll_interval(std::chrono::milliseconds(5000));

    ConfigFileWatcher watcher(watcher_config_file_.string(), [] {});
    watcher.start();
    // Give the watcher thread time to enter its tick wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto stop_started = std::chrono::steady_clock::now();
    watcher.stop();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;
    EXPECT_LT(stop_elapsed, std::chrono::milliseconds(900))
        << "stop must wake the poll-tick wait instead of sitting it out";
}

// make_config(options, old) + reloadConfig() retains non-reloadable fields
// (identity, collector) with a warning, while reloadable fields still apply.
TEST_F(StartAgentTest, ReloadConfigRetainsNonReloadableFields) {
    auto cfg = make_test_config_for_create_agent();
    auto agent = install_mock_agent(cfg);
    ASSERT_TRUE(agent->Enable());

    // Changes non-reloadable fields (app name, collector port) AND a
    // reloadable one (counter rate) at once.
    std::string non_reloadable_config = R"(
ApplicationName: different-app-name
AgentName: test-agent-name
Enable: true
Collector:
  GrpcHost: 127.0.0.1
  GrpcAgentPort: 8888
  GrpcSpanPort: 9993
  GrpcStatPort: 9992
Sampling:
  Type: COUNTER
  CounterRate: 7
)";
    set_config_string(non_reloadable_config);

    start_log_capture();
    auto reload_cfg = make_config(agent->getConfig());
    const auto log = read_captured_log();
    ASSERT_NE(reload_cfg, nullptr);
    agent->reloadConfig(reload_cfg);

    // A non-reloadable change does not block the reload: the running values
    // are retained (with a warning) and the reloadable fields are applied.
    EXPECT_NE(log.find("[warning]"), std::string::npos);
    EXPECT_NE(log.find("non-reloadable config fields changed at runtime"), std::string::npos);
    EXPECT_EQ(agent->getAppName(), "test-app");
    EXPECT_EQ(agent->getConfig()->app_name_, "test-app");
    EXPECT_EQ(agent->getConfig()->collector.agent_port, 9991);
    // The reloadable counter rate still takes effect.
    EXPECT_EQ(agent->getConfig()->sampling.counter_rate, 7);
}

TEST_F(StartAgentTest, ReloadConfigAppliesUrlFilter) {
    // 1. Install a mock agent as the global agent
    auto cfg = make_test_config_for_create_agent();
    auto agent = install_mock_agent(cfg);
    ASSERT_TRUE(agent->Enable());

    // Verify no URL filter initially — /health should produce a real span
    auto span_before = agent->NewSpan("op", "/health");
    ASSERT_NE(span_before, nullptr);

    // 2. Reload with exclude_url added
    std::string config_with_filter = R"(
ApplicationName: test-app
AgentName: test-agent-name
Enable: true
Collector:
  GrpcHost: 127.0.0.1
  GrpcAgentPort: 9991
  GrpcSpanPort: 9993
  GrpcStatPort: 9992
Sampling:
  Type: COUNTER
  CounterRate: 1
Http:
  Server:
    ExcludeUrl:
      - /health
)";
    set_config_string(config_with_filter);
    auto reload_cfg = make_config(agent->getConfig());
    ASSERT_NE(reload_cfg, nullptr);
    agent->reloadConfig(reload_cfg);

    // 3. Verify: /health is now filtered
    auto span_after = agent->NewSpan("op", "/health");
    // Filtered URL returns noop span — EndSpan should not crash
    span_after->EndSpan();
}

TEST_F(StartAgentTest, StartAgentFailsWhenConfigInvalid) {
    // No agent exists yet (SetUp resets the global agent). An invalid config
    // (empty app_name fails check()) must fail the start.
    std::string invalid_config = R"(
ApplicationName: ""
Collector:
  GrpcHost: 127.0.0.1
)";
    set_config_string(invalid_config);

    EXPECT_FALSE(StartAgent()) << "StartAgent() must fail when config is invalid";
    // Nothing was installed: GlobalAgent() keeps serving the noop agent.
    EXPECT_EQ(std::dynamic_pointer_cast<AgentImpl>(GlobalAgent()), nullptr)
        << "no agent must be installed when config is invalid";
}

// --- Shut-down global agent is replaced, never returned ---
//
// Returning a shut-down agent would hand the caller a permanently disabled
// handle: Start() refuses it for good, so it can never come back online.
// Shutdown() clears the singleton under global_agent_mutex before it sets the
// exiting flag, so the normal path never leaves an exiting agent installed —
// but Shutdown() swallows a failure to take that mutex and tears the agent
// down anyway, which does. Installing an already-shut-down agent reproduces
// exactly that state; StartAgent() must evict it and build a fresh agent.

TEST_F(StartAgentTest, StartAgentReplacesShutdownGlobalAgent) {
    auto cfg = make_test_config_for_create_agent();
    auto dead_agent = install_mock_agent(cfg);
    ASSERT_TRUE(dead_agent->Enable());

    // Shutdown() removes it from the singleton; put it back so the singleton
    // holds an agent that is already exiting.
    dead_agent->Shutdown();
    ASSERT_TRUE(dead_agent->isExiting());
    set_global_agent(dead_agent);
    ASSERT_EQ(std::dynamic_pointer_cast<AgentImpl>(GlobalAgent()).get(), dead_agent.get());

    set_config_string(kBaseConfigYaml);
    ASSERT_TRUE(StartAgent()) << "a fresh agent must be built and launched";

    // The singleton was replaced, so GlobalAgent() stops handing out the
    // dead instance.
    auto returned_impl = std::dynamic_pointer_cast<AgentImpl>(GlobalAgent());
    ASSERT_NE(returned_impl, nullptr) << "a fresh agent must be built, not a noop";
    EXPECT_NE(returned_impl.get(), dead_agent.get())
        << "a shut-down agent must never be reused";
    EXPECT_FALSE(returned_impl->isExiting());

    // The old agent stays shut down and inert.
    EXPECT_FALSE(dead_agent->Enable());
    EXPECT_TRUE(dead_agent->isExiting());
}

TEST_F(StartAgentTest, StartAgentEvictsShutdownGlobalAgentEvenWhenRebuildFails) {
    auto cfg = make_test_config_for_create_agent();
    auto dead_agent = install_mock_agent(cfg);
    dead_agent->Shutdown();
    set_global_agent(dead_agent);

    // check() fails on the empty application name, so no replacement is built.
    set_config_string(R"(
ApplicationName: ""
Collector:
  GrpcHost: 127.0.0.1
)");
    EXPECT_FALSE(StartAgent()) << "an invalid config must still fail the start";
    // The dead agent must be gone from the singleton regardless: GlobalAgent()
    // degrading to the noop agent is recoverable, handing out a shut-down
    // agent forever is not.
    EXPECT_EQ(std::dynamic_pointer_cast<AgentImpl>(GlobalAgent()), nullptr)
        << "the shut-down agent must be evicted from the global handle";
}

namespace {

// A GrpcAgent whose channel bring-up throws, failing the agent's ASYNC
// initialization (init_grpc_workers) after Start() already reported a
// successful launch — the window a synchronous Start() failure (which
// StartAgent() handles by not publishing) never enters.
class FailingChannelGrpcAgent : public TestableGrpcAgent {
public:
    using TestableGrpcAgent::TestableGrpcAgent;

protected:
    void create_stub() override {
        throw std::runtime_error("injected channel bring-up failure");
    }
};

// Builds a shared agent whose init thread dies in openChannel(); waits for
// the failure to be recorded so the caller observes the terminal state.
std::shared_ptr<AgentImpl> make_init_failed_agent(const std::shared_ptr<Config>& cfg) {
    auto grpc_agent = std::make_unique<FailingChannelGrpcAgent>(cfg);
    auto grpc_metadata = std::make_unique<TestableGrpcMetadata>(cfg);
    auto grpc_span = std::make_unique<TestableGrpcSpan>(cfg);
    auto grpc_stat = std::make_unique<TestableGrpcStats>(cfg);
    grpc_agent->injectMockStubs();
    grpc_metadata->injectMockStubs();
    grpc_span->injectMockStubs();
    grpc_stat->injectMockStubs();

    auto agent = AgentImpl::createShared(
        cfg, std::move(grpc_agent), std::move(grpc_metadata),
        std::move(grpc_span), std::move(grpc_stat));
    EXPECT_TRUE(agent->Start()) << "the synchronous half of Start() must succeed";

    wait_for_condition([&] { return agent->initFailed(); }, std::chrono::seconds(3),
                       std::chrono::milliseconds(1));
    return agent;
}

}  // namespace

// An agent whose async initialization failed is permanently offline but not
// exiting; StartAgent() used to return it as "the running agent" forever.
// It must be treated like a shut-down agent: evicted and replaced.
TEST_F(StartAgentTest, StartAgentReplacesInitFailedGlobalAgent) {
    auto cfg = make_test_config_for_create_agent();
    auto dead_agent = make_init_failed_agent(cfg);
    ASSERT_TRUE(dead_agent->initFailed());
    EXPECT_FALSE(dead_agent->Enable());
    EXPECT_FALSE(dead_agent->isExiting())
        << "an init failure must not require the agent to be exiting for replacement";
    set_global_agent(dead_agent);

    set_config_string(kBaseConfigYaml);
    ASSERT_TRUE(StartAgent()) << "a fresh agent must be built and launched";

    // The singleton was replaced, so GlobalAgent() stops handing out the
    // dead instance.
    auto returned_impl = std::dynamic_pointer_cast<AgentImpl>(GlobalAgent());
    ASSERT_NE(returned_impl, nullptr) << "a fresh agent must be built, not a noop";
    EXPECT_NE(returned_impl.get(), dead_agent.get())
        << "an init-failed agent must never be kept as the running agent";
    EXPECT_FALSE(returned_impl->initFailed());
}

TEST_F(StartAgentTest, ReloadConfigAppliesMultipleTimes) {
    // 1. Install a mock agent
    auto cfg = make_test_config_for_create_agent();
    auto agent = install_mock_agent(cfg);
    ASSERT_TRUE(agent->Enable());
    EXPECT_EQ(agent->getConfig()->sampling.counter_rate, 1);

    // 2. First reload: change counter rate to 10
    std::string config_v2 = R"(
ApplicationName: test-app
AgentName: test-agent-name
Enable: true
Collector:
  GrpcHost: 127.0.0.1
  GrpcAgentPort: 9991
  GrpcSpanPort: 9993
  GrpcStatPort: 9992
Sampling:
  Type: COUNTER
  CounterRate: 10
)";
    set_config_string(config_v2);
    auto cfg_v2 = make_config(agent->getConfig());
    ASSERT_NE(cfg_v2, nullptr);
    agent->reloadConfig(cfg_v2);
    EXPECT_EQ(agent->getConfig()->sampling.counter_rate, 10);

    // 3. Second reload: change counter rate to 100
    std::string config_v3 = R"(
ApplicationName: test-app
AgentName: test-agent-name
Enable: true
Collector:
  GrpcHost: 127.0.0.1
  GrpcAgentPort: 9991
  GrpcSpanPort: 9993
  GrpcStatPort: 9992
Sampling:
  Type: COUNTER
  CounterRate: 100
)";
    set_config_string(config_v3);
    auto cfg_v3 = make_config(agent->getConfig());
    ASSERT_NE(cfg_v3, nullptr);
    agent->reloadConfig(cfg_v3);
    EXPECT_EQ(agent->getConfig()->sampling.counter_rate, 100);
}

// Environment variables seed only the initial config. When make_config() is
// handed the running config (a reload), it must NOT re-read them, so a value
// set only via env can never override the running config on reload.
TEST_F(StartAgentTest, MakeConfigSkipsEnvVarsOnReload) {
    // First load (no old config): the env var IS applied.
    // env::APPLICATION_NAME is only the suffix; the agent reads "<prefix>_<suffix>".
    const std::string app_name_env = std::string(env::DEFAULT_PREFIX) + "_" + env::APPLICATION_NAME;
    setenv(app_name_env.c_str(), "env-app-name", 1);
    auto cfg_initial = make_config();
    ASSERT_NE(cfg_initial, nullptr);
    const auto app_name_initial = cfg_initial->app_name_;

    // Rebuild for a reload: the env var must now be ignored, and the identity
    // is retained from the running config.
    auto running_cfg = make_test_config();
    auto cfg_reload = make_config(running_cfg);
    ASSERT_NE(cfg_reload, nullptr);

    unsetenv(app_name_env.c_str());

    EXPECT_EQ(app_name_initial, "env-app-name")
        << "env var should seed the initial config";
    EXPECT_EQ(cfg_reload->app_name_, running_cfg->app_name_)
        << "reload must retain the running app name, not re-read the env var";
}

// Env-sourced values still survive reloads: the reload config is seeded from
// the running config (which absorbed the env overrides at first load), so an
// env-forced debug level does not silently revert when an unrelated file
// setting changes — as long as the file does not set the key itself.
TEST_F(StartAgentTest, MakeConfigKeepsEnvSeededLogLevelOnReload) {
    const std::string log_level_env = std::string(env::DEFAULT_PREFIX) + "_" + env::LOG_LEVEL;
    setenv(log_level_env.c_str(), "debug", 1);
    auto cfg_initial = make_config();
    unsetenv(log_level_env.c_str());
    ASSERT_NE(cfg_initial, nullptr);
    ASSERT_EQ(cfg_initial->log.level, "debug");

    // Rebuild for a reload with an empty config string: the level is inherited
    // from the running config, not reset to the "info" default.
    auto cfg_reload = make_config(cfg_initial);
    ASSERT_NE(cfg_reload, nullptr);
    EXPECT_EQ(cfg_reload->log.level, "debug")
        << "env-seeded log settings must survive a config rebuild for reload";
}

// ============================================================
// Injected cache size tests: the metadata caches used to be pinned at 1024
// entries, which made eviction unreachable from agent-level tests. A
// capacity-1 cache (one shard, one entry) makes eviction deterministic.
// ============================================================

// The cache* APIs return 0 until the boot AgentInfo registration enables the
// agent, so each test must wait for enablement first.
static void wait_cache_test_agent_enabled(const std::shared_ptr<AgentImpl>& agent) {
    ASSERT_TRUE(wait_for_condition([&] { return agent->Enable(); }, std::chrono::seconds(3)));
}

TEST(AgentImplCacheSizeTest, ApiCacheEvictionMintsFreshId) {
    auto cfg = make_test_config();
    auto agent = make_test_agent(cfg, 1);
    wait_cache_test_agent_enabled(agent);

    const int32_t first = agent->cacheApi("api.one", 100);
    EXPECT_GT(first, 0);
    // While resident, the same key keeps its id.
    EXPECT_EQ(agent->cacheApi("api.one", 100), first);

    // A second key evicts the first from the capacity-1 cache.
    const int32_t second = agent->cacheApi("api.two", 100);
    EXPECT_NE(second, first);

    // The evicted key is re-registered under a freshly minted id, so the
    // collector receives its metadata again.
    const int32_t reissued = agent->cacheApi("api.one", 100);
    EXPECT_NE(reissued, first);
    EXPECT_NE(reissued, second);

    agent->Shutdown();
}

TEST(AgentImplCacheSizeTest, ErrorAndSqlCacheEvictionMintsFreshIds) {
    auto cfg = make_test_config();
    auto agent = make_test_agent(cfg, 1);
    wait_cache_test_agent_enabled(agent);

    const int32_t first_error = agent->cacheError("ErrorOne");
    agent->cacheError("ErrorTwo");
    EXPECT_NE(agent->cacheError("ErrorOne"), first_error);

    const int32_t first_sql = agent->cacheSql("SELECT 1");
    agent->cacheSql("SELECT 2");
    EXPECT_NE(agent->cacheSql("SELECT 1"), first_sql);

    agent->Shutdown();
}

TEST(AgentImplCacheSizeTest, DefaultCacheSizeKeepsIdsStable) {
    auto cfg = make_test_config();
    auto agent = make_test_agent(cfg);
    wait_cache_test_agent_enabled(agent);

    const int32_t first = agent->cacheApi("api.one", 100);
    EXPECT_GT(first, 0);
    agent->cacheApi("api.two", 100);
    EXPECT_EQ(agent->cacheApi("api.one", 100), first)
        << "with the production capacity no eviction may happen for two keys";

    agent->Shutdown();
}

}  // namespace pinpoint
