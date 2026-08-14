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
#include "../src/agent_service.h"
#include "../src/config.h"
#include "../include/pinpoint/tracer.h"
#include "mock_agent_service.h"

namespace pinpoint {

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
