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
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>

#include "../src/grpc.h"
#include "../src/grpc_builders.h"
#include "../src/callstack.h"
#include "../src/stat.h"
#include "../src/agent_service.h"
#include "../src/config.h"
#include "../include/pinpoint/tracer.h"
#include "mock_agent_service.h"
#include "mock_helpers.h"

namespace pinpoint {

namespace {

// Exposes the transport snapshot and the per-client stop for the channel
// rotation tests below. Uses the real readyChannel() against real channels.
class RotationProbeGrpcSpan : public GrpcSpan {
public:
    using GrpcSpan::GrpcSpan;
    std::shared_ptr<const Transport> transport() const { return current_transport<SpanStub>(); }
    void stop() { request_stop(); }
    // Stream-stall escalation is exercised by driving the same signals the
    // ping/stat workers feed (record_stream_stall / record_stream_write_ok)
    // and then running the real readyChannel() rotation. GrpcSpan has no
    // stream itself, so nothing else touches these counters here.
    void recordStreamStall() { record_stream_stall(); }
    void recordStreamWriteOk() { record_stream_write_ok(); }
    uint32_t generation() const {
        const auto t = transport();
        return t ? t->generation : 0;
    }
};

}  // namespace

class GrpcTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_agent_service_ = std::make_unique<MockAgentService>();
        mock_agent_service_->setExiting(false);
        mock_agent_service_->setStartTime(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        auto& cfg = mock_agent_service_->mutableConfig();
        cfg->span.event_chunk_size = 10;
        cfg->span.max_event_depth = 32;
        cfg->span.queue_size = 1024;
        cfg->http.url_stat.enable = true;
        cfg->http.url_stat.limit = 1024;
        cfg->http.url_stat.trim_path_depth = 3;
        cfg->collector.host = "localhost";
        cfg->collector.agent_port = 9991;
        cfg->collector.span_port = 9993;
        cfg->collector.stat_port = 9992;
        cfg->app_name_ = "test-app";
        cfg->agent_id_ = "test-agent-id";
        cfg->agent_name_ = "test-agent-name";
        mock_agent_service_->setAppName("test-app");
        mock_agent_service_->setAppType(1300);
        mock_agent_service_->setAgentId("test-agent-id");

        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        certificate_path_ = std::filesystem::temp_directory_path() /
            (std::string("pinpoint-grpc-") + std::to_string(getpid()) + "-" +
             info->name() + ".pem");
        std::error_code ec;
        std::filesystem::remove(certificate_path_, ec);
    }

    void TearDown() override {
        mock_agent_service_.reset();
        std::error_code ec;
        std::filesystem::remove(certificate_path_, ec);
    }

    void WriteCertificateFile() const {
        std::ofstream certificate(certificate_path_, std::ios::binary);
        ASSERT_TRUE(certificate.is_open());
        // Channel construction only needs to prove that the selected file is
        // read. Certificate validation happens when a TLS handshake is made.
        certificate << "test-only certificate contents\n";
        ASSERT_TRUE(certificate.good());
    }

    std::unique_ptr<MockAgentService> mock_agent_service_;
    std::filesystem::path certificate_path_;
};

TEST(ExponentialBackoffTest, DelayIncreasesAndCapsWithoutJitter) {
    ExponentialBackoff backoff(std::chrono::milliseconds(1000), 2.0, 0.0, std::chrono::milliseconds(3000));

    EXPECT_EQ(backoff.next_delay(), std::chrono::milliseconds(1000));
    EXPECT_EQ(backoff.next_delay(), std::chrono::milliseconds(2000));
    EXPECT_EQ(backoff.next_delay(), std::chrono::milliseconds(3000));
    EXPECT_EQ(backoff.next_delay(), std::chrono::milliseconds(3000));
}

TEST(ExponentialBackoffTest, ResetStartsFromInitialDelay) {
    ExponentialBackoff backoff(std::chrono::milliseconds(3000), 1.2, 0.0, std::chrono::milliseconds(30000));

    EXPECT_EQ(backoff.next_delay(), std::chrono::milliseconds(3000));
    EXPECT_EQ(backoff.next_delay(), std::chrono::milliseconds(3600));

    backoff.reset();

    EXPECT_EQ(backoff.next_delay(), std::chrono::milliseconds(3000));
}

// The flat-jitter shape used by registerAgentWithRetry(): multiplier 1.0
// keeps the base interval so the delay never escalates, and the jitter alone
// spreads sibling workers' retries apart.
TEST(ExponentialBackoffTest, FlatMultiplierJittersAroundInitialDelay) {
    ExponentialBackoff backoff(std::chrono::milliseconds(1000), 1.0, 0.3,
                               std::chrono::milliseconds(2000));

    for (int i = 0; i < 32; ++i) {
        const auto delay = backoff.next_delay();
        EXPECT_GE(delay, std::chrono::milliseconds(700));
        EXPECT_LE(delay, std::chrono::milliseconds(1300));
    }
}

TEST(ExponentialBackoffTest, JitterStaysWithinRandomizationRange) {
    ExponentialBackoff backoff(std::chrono::milliseconds(3000), 1.2, 0.3, std::chrono::milliseconds(30000));

    for (int i = 0; i < 100; ++i) {
        backoff.reset();
        const auto delay = backoff.next_delay();
        EXPECT_GE(delay, std::chrono::milliseconds(2100));
        EXPECT_LE(delay, std::chrono::milliseconds(3900));
    }
}

TEST_F(GrpcTest, GrpcClientTlsMissingSelectedCertificateThrows) {
    auto& ssl = mock_agent_service_->mutableConfig()->collector.grpc.ssl;
    ssl.enable = true;
    ssl.trust_cert_file_path = certificate_path_.string();

    GrpcAgent client(mock_agent_service_->getConfig());
    client.setAgentService(mock_agent_service_.get());

    EXPECT_THROW(client.openChannel(), std::runtime_error);
}

TEST_F(GrpcTest, GrpcClientTlsPrefersTrustCertificateOverRootCertificate) {
    WriteCertificateFile();
    auto& ssl = mock_agent_service_->mutableConfig()->collector.grpc.ssl;
    ssl.enable = true;
    ssl.trust_cert_file_path = certificate_path_.string();
    ssl.root_cert_file_path = certificate_path_.string() + ".missing-root";

    GrpcAgent client(mock_agent_service_->getConfig());
    client.setAgentService(mock_agent_service_.get());

    EXPECT_NO_THROW(client.openChannel());
    client.closeChannel();
}

TEST_F(GrpcTest, GrpcClientTlsFallsBackToRootCertificate) {
    WriteCertificateFile();
    auto& ssl = mock_agent_service_->mutableConfig()->collector.grpc.ssl;
    ssl.enable = true;
    ssl.trust_cert_file_path.clear();
    ssl.root_cert_file_path = certificate_path_.string();

    GrpcAgent client(mock_agent_service_->getConfig());
    client.setAgentService(mock_agent_service_.get());

    EXPECT_NO_THROW(client.openChannel());
    client.closeChannel();
}

TEST_F(GrpcTest, GrpcClientTlsUsesDefaultRootsWhenNoCertificatePathIsSet) {
    auto& ssl = mock_agent_service_->mutableConfig()->collector.grpc.ssl;
    ssl.enable = true;
    ssl.trust_cert_file_path.clear();
    ssl.root_cert_file_path.clear();

    GrpcAgent client(mock_agent_service_->getConfig());
    client.setAgentService(mock_agent_service_.get());

    EXPECT_NO_THROW(client.openChannel());
    client.closeChannel();
}

// Metadata Structure Tests

TEST_F(GrpcTest, MetaDataApiTest) {
    MetaData meta_data{ApiMeta(1, 100, "test.api")};

    ASSERT_TRUE(std::holds_alternative<ApiMeta>(meta_data));
    const auto& api_meta = std::get<ApiMeta>(meta_data);
    EXPECT_EQ(api_meta.id_, 1);
    EXPECT_EQ(api_meta.type_, 100);
    EXPECT_EQ(api_meta.api_str_, "test.api");
}

TEST_F(GrpcTest, MetaDataStringTest) {
    MetaData meta_data{StringMeta(2, "test.string", STRING_META_ERROR)};

    ASSERT_TRUE(std::holds_alternative<StringMeta>(meta_data));
    const auto& str_meta = std::get<StringMeta>(meta_data);
    EXPECT_EQ(str_meta.id_, 2);
    EXPECT_EQ(str_meta.str_val_, "test.string");
    EXPECT_EQ(str_meta.type_, STRING_META_ERROR);
}

// ExceptionMeta Tests

TEST_F(GrpcTest, ExceptionMetaTest) {
    TraceId txid{"test-agent", 12345, 1};
    std::vector<std::unique_ptr<Exception>> exceptions;
    auto callstack = std::make_unique<CallStack>("test error");
    callstack->push("module", "func", "file.cpp", 42);
    exceptions.push_back(std::make_unique<Exception>(std::move(callstack)));

    ExceptionMeta meta(txid, 9999, "/api/test", std::move(exceptions));

    EXPECT_EQ(meta.txid_.agentId(), "test-agent");
    EXPECT_EQ(meta.txid_.StartTime, 12345);
    EXPECT_EQ(meta.txid_.Sequence, 1);
    EXPECT_EQ(meta.span_id_, 9999);
    EXPECT_EQ(meta.url_template_, "/api/test");
    EXPECT_EQ(meta.exceptions_.size(), 1u);
    EXPECT_EQ(meta.exceptions_[0]->getCallStack().getErrorMessage(), "test error");
}

TEST_F(GrpcTest, ExceptionMetaMoveTest) {
    TraceId txid{"agent", 100, 0};
    std::vector<std::unique_ptr<Exception>> exceptions;
    auto callstack = std::make_unique<CallStack>("error1");
    exceptions.push_back(std::make_unique<Exception>(std::move(callstack)));

    ExceptionMeta meta1(txid, 1, "/url", std::move(exceptions));
    ExceptionMeta meta2(std::move(meta1));

    EXPECT_EQ(meta2.span_id_, 1);
    EXPECT_EQ(meta2.url_template_, "/url");
    EXPECT_EQ(meta2.exceptions_.size(), 1u);
}

TEST_F(GrpcTest, ExceptionMetaMultipleExceptionsTest) {
    TraceId txid{"agent", 100, 0};
    std::vector<std::unique_ptr<Exception>> exceptions;
    for (int i = 0; i < 3; i++) {
        auto cs = std::make_unique<CallStack>("error" + std::to_string(i));
        exceptions.push_back(std::make_unique<Exception>(std::move(cs)));
    }

    ExceptionMeta meta(txid, 1, "/multi", std::move(exceptions));
    EXPECT_EQ(meta.exceptions_.size(), 3u);
}

// Call stacks recorded under one chain id are the links of a single
// exception: depth counts up from 0 in record order and the caller's error
// name — not the top frame's module — is the class name.
TEST_F(GrpcTest, ExceptionMetadataChainDepthTest) {
    TraceId txid{"agent", 100, 7};
    std::vector<std::unique_ptr<Exception>> exceptions;

    auto cause = std::make_unique<CallStack>("root cause", "IOException", 1700000000000);
    cause->push("libdb", "connect", "db.cpp", 11);
    auto first = std::make_unique<Exception>(std::move(cause));
    const auto chain_id = first->getId();
    exceptions.push_back(std::move(first));

    auto wrapper = std::make_unique<CallStack>("wrapped", "SQLException", 1700000000000);
    wrapper->push("libapp", "query", "app.cpp", 22);
    exceptions.push_back(std::make_unique<Exception>(std::move(wrapper), chain_id));

    google::protobuf::Arena arena;
    const auto* wire = build_exception_metadata(txid, 42, "/api/*", exceptions, &arena);
    ASSERT_NE(wire, nullptr);
    ASSERT_EQ(wire->exceptions_size(), 2);

    EXPECT_EQ(wire->exceptions(0).exceptionid(), chain_id);
    EXPECT_EQ(wire->exceptions(0).exceptiondepth(), 0);
    EXPECT_EQ(wire->exceptions(0).exceptionclassname(), "IOException");
    EXPECT_EQ(wire->exceptions(0).exceptionmessage(), "root cause");
    EXPECT_EQ(wire->exceptions(0).starttime(), 1700000000000);

    EXPECT_EQ(wire->exceptions(1).exceptionid(), chain_id);
    EXPECT_EQ(wire->exceptions(1).exceptiondepth(), 1);
    EXPECT_EQ(wire->exceptions(1).exceptionclassname(), "SQLException");
}

// Unrelated exceptions each start their own chain at depth 0, and a call
// stack recorded without an error name keeps the top-frame fallback.
TEST_F(GrpcTest, ExceptionMetadataIndependentExceptionsTest) {
    TraceId txid{"agent", 100, 8};
    std::vector<std::unique_ptr<Exception>> exceptions;
    for (int i = 0; i < 2; i++) {
        auto cs = std::make_unique<CallStack>("error" + std::to_string(i));
        cs->push("libmod", "fn", "f.cpp", i);
        exceptions.push_back(std::make_unique<Exception>(std::move(cs)));
    }

    google::protobuf::Arena arena;
    const auto* wire = build_exception_metadata(txid, 1, "/multi", exceptions, &arena);
    ASSERT_EQ(wire->exceptions_size(), 2);

    EXPECT_NE(wire->exceptions(0).exceptionid(), wire->exceptions(1).exceptionid());
    EXPECT_EQ(wire->exceptions(0).exceptiondepth(), 0);
    EXPECT_EQ(wire->exceptions(1).exceptiondepth(), 0);
    EXPECT_EQ(wire->exceptions(0).exceptionclassname(), "libmod")
        << "no error name falls back to the top frame's module";
    EXPECT_GT(wire->exceptions(0).starttime(), 0)
        << "no supplied start time stamps the wall clock";
}

// MetaData with SQL UID and Exception types

TEST_F(GrpcTest, MetaDataSqlUidTest) {
    SqlUid uid = {1, 2, 3, 4, 5};
    MetaData meta_data{SqlUidMeta(uid, "SELECT * FROM users")};

    ASSERT_TRUE(std::holds_alternative<SqlUidMeta>(meta_data));
    const auto& sql_uid_meta = std::get<SqlUidMeta>(meta_data);
    EXPECT_EQ(sql_uid_meta.uid_, uid);
    EXPECT_EQ(sql_uid_meta.sql_, "SELECT * FROM users");
}

TEST_F(GrpcTest, MetaDataExceptionTest) {
    TraceId txid{"agent", 100, 5};
    std::vector<std::unique_ptr<Exception>> exceptions;
    auto cs = std::make_unique<CallStack>("null pointer");
    cs->push("libcore", "deref", "ptr.cpp", 10);
    exceptions.push_back(std::make_unique<Exception>(std::move(cs)));

    MetaData meta_data{ExceptionMeta(txid, 42, "/api/v1/resource", std::move(exceptions))};

    ASSERT_TRUE(std::holds_alternative<ExceptionMeta>(meta_data));
    const auto& exc_meta = std::get<ExceptionMeta>(meta_data);
    EXPECT_EQ(exc_meta.txid_.agentId(), "agent");
    EXPECT_EQ(exc_meta.span_id_, 42);
    EXPECT_EQ(exc_meta.url_template_, "/api/v1/resource");
    EXPECT_EQ(exc_meta.exceptions_.size(), 1u);
}

// ============================================================
// Collector outage tests: real channel to an unreachable endpoint.
// Port 1 (tcpmux) is reserved and needs root to bind, so nothing
// listens there and every connection attempt is refused — the same
// channel behavior a collector outage produces.
// ============================================================

TEST_F(GrpcTest, GrpcClientReadyChannelWaitsThroughOutageUntilStopRequested) {
    mock_agent_service_->mutableConfig()->collector.agent_port = 1;

    GrpcAgent client(mock_agent_service_->getConfig());
    client.setAgentService(mock_agent_service_.get());
    client.openChannel();

    auto ready = std::async(std::launch::async, [&client] { return client.readyChannel(); });

    // While the collector stays down, readyChannel() must keep backing off
    // and retrying instead of giving up.
    EXPECT_EQ(ready.wait_for(std::chrono::milliseconds(500)), std::future_status::timeout)
        << "readyChannel should keep waiting while the collector is unreachable";

    // A stop request must interrupt the unbounded retry loop promptly (the
    // wait polls the stop flag every backoff slice, at most 1s).
    client.stopPingWorker();
    ASSERT_EQ(ready.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "stopping the client must interrupt the channel wait";
    EXPECT_FALSE(ready.get());

    // A stopped client is terminal: later readiness checks fail fast instead
    // of re-entering the outage wait.
    const auto recheck_start = std::chrono::steady_clock::now();
    EXPECT_FALSE(client.readyChannel());
    EXPECT_LT(std::chrono::steady_clock::now() - recheck_start, std::chrono::seconds(1));

    client.closeChannel();
}

TEST_F(GrpcTest, GrpcClientReadyChannelRefusesToWaitWhenAgentIsExiting) {
    mock_agent_service_->mutableConfig()->collector.agent_port = 1;

    GrpcAgent client(mock_agent_service_->getConfig());
    client.setAgentService(mock_agent_service_.get());
    client.openChannel();
    mock_agent_service_->setExiting(true);

    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(client.readyChannel());
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1))
        << "readyChannel must not enter the outage backoff once the agent is exiting";

    client.closeChannel();
}

// ========== Connection renewal: jitter and channel rotation ==========

TEST(RandomizeIntervalTest, StaysWithinFactorBoundsAndVaries) {
    std::mt19937_64 rng(42);
    const auto base = std::chrono::milliseconds(1000);
    auto lo = base;
    auto hi = base;
    for (int i = 0; i < 1000; ++i) {
        const auto value = randomize_interval(base, 0.1, rng);
        EXPECT_GE(value.count(), 900);
        EXPECT_LE(value.count(), 1100);
        lo = std::min(lo, value);
        hi = std::max(hi, value);
    }
    EXPECT_LT(lo, base) << "jitter must spread below the base interval";
    EXPECT_GT(hi, base) << "jitter must spread above the base interval";
}

TEST(RandomizeIntervalTest, NoJitterWhenFactorOrIntervalIsZero) {
    std::mt19937_64 rng(7);
    EXPECT_EQ(randomize_interval(std::chrono::milliseconds(1000), 0.0, rng).count(), 1000);
    EXPECT_EQ(randomize_interval(std::chrono::milliseconds(1000), -1.0, rng).count(), 1000);
    EXPECT_EQ(randomize_interval(std::chrono::milliseconds(0), 0.1, rng).count(), 0);
}

// Default configuration: rotation code never publishes anything, so the
// transport openChannel() built is the one every readiness check returns.
TEST_F(GrpcTest, ChannelRotationIsInertWhenDisabled) {
    BareGrpcServer server;
    ASSERT_NE(server.server, nullptr);
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.host = "127.0.0.1";
    cfg->collector.span_port = server.port;
    ASSERT_EQ(cfg->collector.grpc.channel.channel_max_age_ms, 0) << "rotation must be off by default";

    RotationProbeGrpcSpan client(mock_agent_service_->getConfig());
    client.setAgentService(mock_agent_service_.get());
    client.openChannel();
    const auto first = client.transport();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->generation, 1u);

    ASSERT_TRUE(client.readyChannel());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ASSERT_TRUE(client.readyChannel());
    EXPECT_EQ(client.transport(), first) << "with rotation disabled the transport must never change";

    client.closeChannel();
    EXPECT_EQ(client.transport(), nullptr);
}

// Make-before-break against a live server: once the channel is older than its
// max age, readyChannel() publishes a READY successor with a new generation
// and leaves the previous transport intact for whoever still holds it.
TEST_F(GrpcTest, ChannelRotationReplacesAgedChannelOnceSuccessorIsReady) {
    BareGrpcServer server;
    ASSERT_NE(server.server, nullptr);
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.host = "127.0.0.1";
    cfg->collector.span_port = server.port;
    cfg->collector.grpc.channel.channel_max_age_ms = 20;

    GrpcClientTuning tuning;
    tuning.channel_rotation_ready_timeout = std::chrono::seconds(5);
    RotationProbeGrpcSpan client(mock_agent_service_->getConfig(), tuning);
    client.setAgentService(mock_agent_service_.get());
    client.openChannel();
    const auto first = client.transport();
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(client.readyChannel());
    EXPECT_EQ(client.transport(), first) << "a channel younger than its max age is kept";

    // Past 20ms +-10%.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(client.readyChannel());
    const auto second = client.transport();
    ASSERT_NE(second, nullptr);
    EXPECT_NE(second, first) << "an aged channel must be replaced";
    EXPECT_EQ(second->generation, 2u);
    EXPECT_GT(second->created_at, first->created_at);
    ASSERT_NE(second->channel, nullptr);
    EXPECT_EQ(second->channel->GetState(false), GRPC_CHANNEL_READY)
        << "make-before-break: the successor is published only once READY";
    ASSERT_NE(first->channel, nullptr);
    EXPECT_EQ(first->channel->GetState(false), GRPC_CHANNEL_READY)
        << "the previous channel is left alone for the calls still on it";

    client.closeChannel();
}

// Collector down (port 1 refuses connections): neither the current channel
// nor a successor can become READY. The rotation spends at most its timeout
// on the successor and keeps the current transport; readyChannel() then
// enters its usual outage wait, which a stop request interrupts.
TEST_F(GrpcTest, ChannelRotationKeepsCurrentChannelWhenSuccessorNeverReady) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_port = 1;
    cfg->collector.grpc.channel.channel_max_age_ms = 1;

    GrpcClientTuning tuning;
    tuning.channel_rotation_ready_timeout = std::chrono::milliseconds(300);
    tuning.backoff_sleep_slice = std::chrono::milliseconds(50);
    tuning.reconnect_initial_interval = std::chrono::milliseconds(100);
    tuning.reconnect_max_interval = std::chrono::milliseconds(100);
    RotationProbeGrpcSpan client(mock_agent_service_->getConfig(), tuning);
    client.setAgentService(mock_agent_service_.get());
    client.openChannel();
    const auto first = client.transport();
    ASSERT_NE(first, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    auto ready = std::async(std::launch::async, [&client] { return client.readyChannel(); });
    // 300ms of successor wait, then the outage backoff on the current channel.
    EXPECT_EQ(ready.wait_for(std::chrono::milliseconds(500)), std::future_status::timeout);
    client.stop();
    ASSERT_EQ(ready.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_FALSE(ready.get());
    EXPECT_EQ(client.transport(), first)
        << "an unready successor must never replace the current transport";
    EXPECT_EQ(first->generation, 1u);

    client.closeChannel();
}

// Shutdown racing a rotation: the successor wait is long, but a stop request
// ends it within one backoff slice, publishing nothing.
TEST_F(GrpcTest, ChannelRotationAbandonsWaitWhenStopRequested) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_port = 1;
    cfg->collector.grpc.channel.channel_max_age_ms = 1;

    GrpcClientTuning tuning;
    tuning.channel_rotation_ready_timeout = std::chrono::seconds(30);
    tuning.backoff_sleep_slice = std::chrono::milliseconds(50);
    RotationProbeGrpcSpan client(mock_agent_service_->getConfig(), tuning);
    client.setAgentService(mock_agent_service_.get());
    client.openChannel();
    const auto first = client.transport();
    ASSERT_NE(first, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    auto ready = std::async(std::launch::async, [&client] { return client.readyChannel(); });
    // Well inside the 30s successor wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto stop_at = std::chrono::steady_clock::now();
    client.stop();
    ASSERT_EQ(ready.wait_for(std::chrono::seconds(2)), std::future_status::ready)
        << "a stop must interrupt the rotation wait";
    EXPECT_LT(std::chrono::steady_clock::now() - stop_at, std::chrono::seconds(1));
    EXPECT_FALSE(ready.get());
    EXPECT_EQ(client.transport(), first);

    client.closeChannel();
}

// ========== Stream-stall escalation: forced channel rotation ==========
//
// The ping/stat workers call record_stream_stall() on every write timeout
// (channel healthy, backend not reading) and record_stream_write_ok() on a
// completed write. These drive the same real readyChannel() rotation path the
// age-based renewal uses. A short stall window and a live BareGrpcServer let
// the successor reach READY so the forced rotation actually publishes.

// (a) Under the count limit, and (below) under the time limit, readyChannel()
//     must not rotate: the stream is recycled but the channel is untouched.
TEST_F(GrpcTest, StreamStallBelowLimitDoesNotRotateChannel) {
    BareGrpcServer server;
    ASSERT_NE(server.server, nullptr);
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.host = "127.0.0.1";
    cfg->collector.span_port = server.port;
    ASSERT_EQ(cfg->collector.grpc.channel.channel_max_age_ms, 0) << "age rotation must stay off";

    GrpcClientTuning tuning;
    tuning.stream_stall_limit_count = 3;
    tuning.stream_stall_limit_time = std::chrono::milliseconds(0);  // isolate the count condition
    RotationProbeGrpcSpan client(mock_agent_service_->getConfig(), tuning);
    client.setAgentService(mock_agent_service_.get());
    client.openChannel();
    ASSERT_EQ(client.generation(), 1u);

    // Two stalls: one short of the three-count limit.
    client.recordStreamStall();
    ASSERT_TRUE(client.readyChannel());
    client.recordStreamStall();
    ASSERT_TRUE(client.readyChannel());
    EXPECT_EQ(client.generation(), 1u) << "below the count limit the channel must be kept";

    client.closeChannel();
}

// The time condition alone: enough stalls, but not yet spanning the window.
TEST_F(GrpcTest, StreamStallBeforeTimeWindowDoesNotRotateChannel) {
    BareGrpcServer server;
    ASSERT_NE(server.server, nullptr);
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.host = "127.0.0.1";
    cfg->collector.span_port = server.port;

    GrpcClientTuning tuning;
    tuning.stream_stall_limit_count = 1;
    tuning.stream_stall_limit_time = std::chrono::seconds(30);  // never elapses in-test
    RotationProbeGrpcSpan client(mock_agent_service_->getConfig(), tuning);
    client.setAgentService(mock_agent_service_.get());
    client.openChannel();
    ASSERT_EQ(client.generation(), 1u);

    client.recordStreamStall();
    ASSERT_TRUE(client.readyChannel());
    EXPECT_EQ(client.generation(), 1u)
        << "the count is met but the stall window has not elapsed: no rotation";

    client.closeChannel();
}

// (b) Both limits satisfied: readyChannel() forces a make-before-break
//     rotation to a fresh generation, even with age rotation disabled.
TEST_F(GrpcTest, StreamStallAtLimitForcesChannelRotation) {
    BareGrpcServer server;
    ASSERT_NE(server.server, nullptr);
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.host = "127.0.0.1";
    cfg->collector.span_port = server.port;
    ASSERT_EQ(cfg->collector.grpc.channel.channel_max_age_ms, 0)
        << "the forced rotation must work with age rotation disabled";

    GrpcClientTuning tuning;
    tuning.stream_stall_limit_count = 3;
    tuning.stream_stall_limit_time = std::chrono::milliseconds(20);
    tuning.channel_rotation_ready_timeout = std::chrono::seconds(5);
    RotationProbeGrpcSpan client(mock_agent_service_->getConfig(), tuning);
    client.setAgentService(mock_agent_service_.get());
    client.openChannel();
    const auto first = client.transport();
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(first->generation, 1u);

    // Three stalls spanning the 20ms window.
    client.recordStreamStall();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    client.recordStreamStall();
    client.recordStreamStall();
    ASSERT_TRUE(client.readyChannel());

    const auto second = client.transport();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->generation, 2u) << "the stalled channel must be replaced";
    EXPECT_NE(second, first);
    ASSERT_NE(second->channel, nullptr);
    EXPECT_EQ(second->channel->GetState(false), GRPC_CHANNEL_READY)
        << "make-before-break: the successor is published only once READY";

    // The evidence is consumed: an immediately following readyChannel() with
    // no new stalls must not rotate again.
    ASSERT_TRUE(client.readyChannel());
    EXPECT_EQ(client.generation(), 2u) << "a forced rotation resets the stall count";

    client.closeChannel();
}

// (c) A completed write resets the count: stalls that would otherwise reach
//     the limit are forgotten once the backend answers.
TEST_F(GrpcTest, StreamWriteOkResetsStallCount) {
    BareGrpcServer server;
    ASSERT_NE(server.server, nullptr);
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.host = "127.0.0.1";
    cfg->collector.span_port = server.port;

    GrpcClientTuning tuning;
    tuning.stream_stall_limit_count = 3;
    tuning.stream_stall_limit_time = std::chrono::milliseconds(0);
    RotationProbeGrpcSpan client(mock_agent_service_->getConfig(), tuning);
    client.setAgentService(mock_agent_service_.get());
    client.openChannel();
    ASSERT_EQ(client.generation(), 1u);

    client.recordStreamStall();
    client.recordStreamStall();
    client.recordStreamWriteOk();  // backend read a write: clear the streak
    client.recordStreamStall();
    client.recordStreamStall();
    ASSERT_TRUE(client.readyChannel());
    EXPECT_EQ(client.generation(), 1u)
        << "a completed write between stalls must prevent the limit being reached";

    client.closeChannel();
}

// (d) The successor never becomes READY (collector down): the current channel
//     is kept, and the consumed stall count means the worker does not spin on
//     rotation attempts — a fresh limit's worth of stalls is needed again.
TEST_F(GrpcTest, StreamStallKeepsCurrentChannelWhenSuccessorNeverReady) {
    // Point at a refused port so no successor can connect.
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_port = 1;

    GrpcClientTuning tuning;
    tuning.stream_stall_limit_count = 1;
    tuning.stream_stall_limit_time = std::chrono::milliseconds(0);
    tuning.channel_rotation_ready_timeout = std::chrono::milliseconds(200);
    tuning.backoff_sleep_slice = std::chrono::milliseconds(50);
    RotationProbeGrpcSpan client(mock_agent_service_->getConfig(), tuning);
    client.setAgentService(mock_agent_service_.get());
    client.openChannel();
    const auto first = client.transport();
    ASSERT_NE(first, nullptr);

    // Force the rotation from the worker's angle: readyChannel() runs the
    // stall-forced rotate, whose successor never reaches READY. Since the
    // current channel is a refused port too, readyChannel()'s own readiness
    // wait would then block, so drive rotate_channel_if_due() directly by
    // asserting the outcome on the transport rather than readyChannel()'s
    // return. Run it off-thread and stop it out of the readiness wait.
    client.recordStreamStall();
    auto ready = std::async(std::launch::async, [&client] { return client.readyChannel(); });
    // 200ms successor wait, then the outage backoff on the current channel.
    EXPECT_EQ(ready.wait_for(std::chrono::milliseconds(400)), std::future_status::timeout);
    client.stop();
    ASSERT_EQ(ready.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_FALSE(ready.get());
    EXPECT_EQ(client.transport(), first)
        << "an unready successor must never replace the current transport";
    EXPECT_EQ(first->generation, 1u);

    client.closeChannel();
}

TEST_F(GrpcTest, GrpcAgentRegisterAgentFailsWhenCollectorUnreachable) {
    mock_agent_service_->mutableConfig()->collector.agent_port = 1;

    GrpcAgent agent(mock_agent_service_->getConfig());
    agent.setAgentService(mock_agent_service_.get());
    agent.openChannel();

    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(agent.registerAgent(), SEND_FAIL)
        << "AgentInfo sent into an outage must report SEND_FAIL, not hang";
    // Bounded by the 5s per-request deadline; a refused connection should
    // surface much sooner.
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(8));

    agent.closeChannel();
}

// ========== gRPC metadata header set (per uid version) ==========

namespace {

std::map<std::string, std::string> metadata_map(const Config& config,
                                                int64_t start_time,
                                                int32_t app_type = APP_TYPE_CPP) {
    std::map<std::string, std::string> m;
    for (const auto& [k, v] : build_grpc_metadata(config, config.agent_id_, start_time, app_type)) {
        m[k] = v;
    }
    return m;
}

} // namespace

TEST(GrpcMetadataTest, V1V3CommonHeaders) {
    Config config;
    config.app_name_ = "my-app";
    config.agent_id_ = "agent-1";
    config.agent_name_ = "agent-name";
    config.object_name_version_ = 1; // v1/v3

    const auto m = metadata_map(config, 12345);
    EXPECT_EQ(m.at("applicationname"), "my-app");
    EXPECT_EQ(m.at("agentid"), "agent-1");
    EXPECT_EQ(m.at("agentname"), "agent-name");
    EXPECT_EQ(m.at("starttime"), "12345");
    EXPECT_EQ(m.at("servicetype"), "1300");
    EXPECT_EQ(m.at("protocol.version"), "100");
    // v4-only headers must not be present for v1/v3.
    EXPECT_EQ(m.count("servicename"), 0u);
    EXPECT_EQ(m.count("apikey"), 0u);
}

TEST(GrpcMetadataTest, V1V3AgentNameOmittedWhenEmpty) {
    Config config;
    config.app_name_ = "my-app";
    config.agent_id_ = "agent-1";
    config.object_name_version_ = 1;
    // agent_name_ left empty

    const auto m = metadata_map(config, 1);
    EXPECT_EQ(m.count("agentname"), 0u);
    EXPECT_EQ(m.at("protocol.version"), "100");
}

TEST(GrpcMetadataTest, V4Headers) {
    Config config;
    config.app_name_ = "my-app";
    config.agent_id_ = "AZLxoH6LfD2fLhorPE1ebw";
    config.agent_name_ = "agent-name";
    config.service_name_ = "my-service";
    config.api_key_ = "secret-key";
    config.object_name_version_ = 4;

    const auto m = metadata_map(config, 999);
    EXPECT_EQ(m.at("protocol.version"), "400");
    EXPECT_EQ(m.at("agentname"), "agent-name"); // always sent for v4
    EXPECT_EQ(m.at("servicename"), "my-service");
    EXPECT_EQ(m.at("apikey"), "secret-key");
    EXPECT_EQ(m.at("agentid"), "AZLxoH6LfD2fLhorPE1ebw");
}

// The JVM non-heap pools and the old-generation GC counters have no C++
// source, so they must travel as the uncollected sentinel (-1, Java's
// UNCOLLECTED_VALUE) and not as 0: PJvmGc's fields are proto3
// implicit-presence scalars, so an unset field reads back as 0 on the
// collector and gets stored and plotted as a real measurement.
TEST(GrpcAgentStatBuilderTest, UncollectedJvmFieldsTravelAsMinusOne) {
    AgentStatsSnapshot stat;
    stat.sample_time_ = 1700000000000;
    stat.interval_ = 5123;
    stat.heap_alloc_size_ = 32768;
    stat.heap_max_size_ = 65536;

    google::protobuf::Arena arena;
    const auto* batch = build_agent_stat_batch({stat}, &arena);

    ASSERT_EQ(batch->agentstat_size(), 1);
    const auto& agent_stat = batch->agentstat(0);
    EXPECT_EQ(agent_stat.collectinterval(), 5123) << "the measured interval must reach the wire";

    const auto& gc = agent_stat.gc();
    EXPECT_EQ(gc.jvmmemoryheapused(), 32768) << "resident memory is collected, so it is sent as-is";
    EXPECT_EQ(gc.jvmmemoryheapmax(), 65536);
    EXPECT_EQ(gc.jvmmemorynonheapused(), UNCOLLECTED_STAT_VALUE);
    EXPECT_EQ(gc.jvmmemorynonheapmax(), UNCOLLECTED_STAT_VALUE);
    EXPECT_EQ(gc.jvmgcoldcount(), UNCOLLECTED_STAT_VALUE);
    EXPECT_EQ(gc.jvmgcoldtime(), UNCOLLECTED_STAT_VALUE);
    EXPECT_EQ(gc.type(), v1::JVM_GC_TYPE_UNKNOWN);

    // Default-constructed snapshot: the fd reading never happened, so the
    // field must carry the sentinel rather than 0.
    ASSERT_TRUE(agent_stat.has_filedescriptor());
    EXPECT_EQ(agent_stat.filedescriptor().openfiledescriptorcount(), UNCOLLECTED_STAT_VALUE);
}

// The "Open File Descriptor" chart stayed empty because build_agent_stat never
// populated the submessage at all; a collected count has to reach the wire.
TEST(GrpcAgentStatBuilderTest, OpenFileDescriptorCountReachesTheWire) {
    AgentStatsSnapshot stat;
    stat.open_fd_count_ = 137;

    google::protobuf::Arena arena;
    const auto* batch = build_agent_stat_batch({stat}, &arena);

    ASSERT_EQ(batch->agentstat_size(), 1);
    ASSERT_TRUE(batch->agentstat(0).has_filedescriptor());
    EXPECT_EQ(batch->agentstat(0).filedescriptor().openfiledescriptorcount(), 137);
}

TEST(GrpcMetadataTest, SocketIdNeverInBaseHeaderSet) {
    Config config;
    config.app_name_ = "my-app";
    config.agent_id_ = "agent-1";
    config.object_name_version_ = 1;

    // The socketid header is appended per-context by build_grpc_context(),
    // never by the socket-id-independent base header set.
    EXPECT_EQ(metadata_map(config, 1).count("socketid"), 0u);
}

} // namespace pinpoint
