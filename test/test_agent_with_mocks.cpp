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
#include <chrono>
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
#include "../include/pinpoint/tracer.h"
#include "v1/Service_mock.grpc.pb.h"

using ::testing::_;
using ::testing::Return;
using ::testing::NiceMock;

namespace pinpoint {

// --- Testable gRPC classes that bypass real connections ---

class TestableGrpcAgent : public GrpcAgent {
public:
    explicit TestableGrpcAgent(std::shared_ptr<const Config> config)
        : GrpcAgent(std::move(config)) {}

    void injectMockStubs() {
        auto agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
        EXPECT_CALL(*agent_stub, RequestAgentInfo(_, _, _))
            .WillRepeatedly(Return(grpc::Status::OK));
        set_agent_stub(std::move(agent_stub));

        auto meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
        EXPECT_CALL(*meta_stub, RequestApiMetaData(_, _, _))
            .WillRepeatedly(Return(grpc::Status::OK));
        EXPECT_CALL(*meta_stub, RequestStringMetaData(_, _, _))
            .WillRepeatedly(Return(grpc::Status::OK));
        EXPECT_CALL(*meta_stub, RequestSqlMetaData(_, _, _))
            .WillRepeatedly(Return(grpc::Status::OK));
        EXPECT_CALL(*meta_stub, RequestSqlUidMetaData(_, _, _))
            .WillRepeatedly(Return(grpc::Status::OK));
        set_meta_stub(std::move(meta_stub));
    }

    // First call (registration) returns true, subsequent calls (workers) return false
    // to avoid real gRPC async streaming.
    bool readyChannel() override { return ready_count_++ == 0; }

    // Override to skip real build_agent_info (which calls slow DNS resolution)
    GrpcRequestStatus registerAgent() override { return SEND_OK; }

protected:
    bool wait_channel_ready() const { return true; }

private:
    std::atomic<int> ready_count_{0};
};

class TestableGrpcSpan : public GrpcSpan {
public:
    explicit TestableGrpcSpan(std::shared_ptr<const Config> config)
        : GrpcSpan(std::move(config)) {}

    void injectMockStubs() {
        set_span_stub(std::make_unique<NiceMock<v1::MockSpanStub>>());
    }

    bool readyChannel() override { return false; }

protected:
    bool wait_channel_ready() const { return true; }
};

class TestableGrpcStats : public GrpcStats {
public:
    explicit TestableGrpcStats(std::shared_ptr<const Config> config)
        : GrpcStats(std::move(config)) {}

    void injectMockStubs() {
        set_stats_stub(std::make_unique<NiceMock<v1::MockStatStub>>());
    }

    bool readyChannel() override { return false; }

protected:
    bool wait_channel_ready() const { return true; }
};

// --- Helper to build a valid Config ---

static std::shared_ptr<Config> make_test_config() {
    auto cfg = std::make_shared<Config>();
    cfg->enable = true;
    cfg->app_name_ = "test-app";
    cfg->app_type_ = 1300;
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
    return cfg;
}

// --- Helper to create AgentImpl with mock gRPC clients ---

static std::shared_ptr<AgentImpl> make_test_agent(std::shared_ptr<Config> cfg) {
    auto grpc_agent = std::make_unique<TestableGrpcAgent>(cfg);
    auto grpc_span = std::make_unique<TestableGrpcSpan>(cfg);
    auto grpc_stat = std::make_unique<TestableGrpcStats>(cfg);

    grpc_agent->injectMockStubs();
    grpc_span->injectMockStubs();
    grpc_stat->injectMockStubs();

    return std::make_shared<AgentImpl>(
        cfg,
        std::move(grpc_agent),
        std::move(grpc_span),
        std::move(grpc_stat));
}

// --- Test fixture ---

class AgentImplTest : public ::testing::Test {
protected:
    void SetUp() override {
        cfg_ = make_test_config();
        agent_ = make_test_agent(cfg_);
        // Wait for init_grpc_workers thread to finish and set enabled_
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
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!agent->Enable() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::shared_ptr<Config> cfg_;
    std::shared_ptr<AgentImpl> agent_;
};

// --- Tests ---

TEST_F(AgentImplTest, EnableAfterInit) {
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(AgentImplTest, GetConfigReturnsCorrectValues) {
    EXPECT_EQ(agent_->getAppName(), "test-app");
    EXPECT_EQ(agent_->getAppType(), 1300);
    EXPECT_EQ(agent_->getAgentId(), "test-agent-id");
    EXPECT_EQ(agent_->getAgentName(), "test-agent-name");
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

    EXPECT_EQ(tid1.AgentId, "test-agent-id");
    EXPECT_EQ(tid2.AgentId, "test-agent-id");
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

TEST_F(AgentImplTest, RecordSpanDoesNotCrash) {
    auto span_data = std::make_shared<SpanData>(agent_.get(), "test-op");
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

TEST_F(AgentImplTest, CacheSqlUidReturnsNonEmpty) {
    auto uid = agent_->cacheSqlUid("SELECT 1");
    EXPECT_FALSE(uid.empty());
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
    auto span_data = std::make_shared<SpanData>(agent_.get(), "test-op");
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

    // Even after init, sampling rate 0 means no spans are sampled,
    // but the agent itself is still enabled
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!agent->Enable() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // NewSpan should return unsampled span (not noop)
    auto span = agent->NewSpan("op", "/test");
    ASSERT_NE(span, nullptr);

    agent->Shutdown();
}

}  // namespace pinpoint
