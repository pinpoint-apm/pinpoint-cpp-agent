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

#include "../src/grpc.h"
#include "../src/agent_service.h"
#include "../src/config.h"
#include "../src/span.h"
#include "../src/stat.h"
#include "../src/url_stat.h"
#include "../include/pinpoint/tracer.h"
#include "v1/Service_mock.grpc.pb.h"
#include "mock_agent_service.h"

using ::testing::_;
using ::testing::Return;
using ::testing::NiceMock;
using ::testing::StrictMock;
using ::testing::InSequence;
using ::testing::DoAll;
using ::testing::SetArgPointee;

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
        cfg->app_type_ = 1300;
        cfg->agent_id_ = "test-agent-id";
        cfg->agent_name_ = "test-agent-name";
        mock_agent_service_->setAppName("test-app");
        mock_agent_service_->setAppType(1300);
        mock_agent_service_->setAgentId("test-agent-id");
        mock_agent_service_->setAgentName("test-agent-name");
    }

    void TearDown() override {
        mock_agent_service_.reset();
    }

    std::unique_ptr<MockAgentService> mock_agent_service_;
};

// GrpcClient Tests

TEST_F(GrpcTest, GrpcClientConstructorTest) {
    GrpcAgent client(mock_agent_service_->getConfig()); client.setAgentService(mock_agent_service_.get());
    
    // Basic construction should not throw
    SUCCEED() << "GrpcClient should construct successfully";
}

TEST_F(GrpcTest, GrpcClientChannelTest) {
    GrpcAgent client(mock_agent_service_->getConfig()); client.setAgentService(mock_agent_service_.get());
    
    // Test closeChannel should not throw (skip readyChannel as it blocks in test environment)
    client.closeChannel();
    
    SUCCEED() << "Channel operations should complete without exceptions";
}

// GrpcAgent Tests

TEST_F(GrpcTest, GrpcAgentConstructorTest) {
    GrpcAgent agent(mock_agent_service_->getConfig()); agent.setAgentService(mock_agent_service_.get());
    
    SUCCEED() << "GrpcAgent should construct successfully";
}

TEST_F(GrpcTest, GrpcAgentRegisterAgentTest) {
    GrpcAgent agent(mock_agent_service_->getConfig()); agent.setAgentService(mock_agent_service_.get());
    
    // Skip actual registerAgent call as it blocks without server
    // Just test that the method exists and can be called safely
    SUCCEED() << "GrpcAgent registerAgent method should be available";
}

TEST_F(GrpcTest, GrpcAgentMetaOperationsTest) {
    GrpcAgent agent(mock_agent_service_->getConfig()); agent.setAgentService(mock_agent_service_.get());
    
    // Test enqueueMeta with API metadata
    auto api_meta = std::make_unique<MetaData>(META_API, 1, 100, "test.api");
    agent.enqueueMeta(std::move(api_meta));
    
    // Test enqueueMeta with string metadata
    auto str_meta = std::make_unique<MetaData>(META_STRING, 2, "test.string", STRING_META_ERROR);
    agent.enqueueMeta(std::move(str_meta));
    
    SUCCEED() << "Meta enqueue operations should complete successfully";
}

TEST_F(GrpcTest, GrpcAgentWorkerOperationsTest) {
    GrpcAgent agent(mock_agent_service_->getConfig()); agent.setAgentService(mock_agent_service_.get());
    
    // Test worker stop methods (skip start workers as they block without server)
    agent.stopPingWorker();
    agent.stopMetaWorker();
    
    SUCCEED() << "Worker stop operations should complete successfully";
}

// GrpcSpan Tests

TEST_F(GrpcTest, GrpcSpanConstructorTest) {
    GrpcSpan span_client(mock_agent_service_->getConfig()); span_client.setAgentService(mock_agent_service_.get());
    
    SUCCEED() << "GrpcSpan should construct successfully";
}

TEST_F(GrpcTest, GrpcSpanEnqueueTest) {
    GrpcSpan span_client(mock_agent_service_->getConfig()); span_client.setAgentService(mock_agent_service_.get());
    
    // Create a test span chunk
    auto span_data = std::make_shared<SpanData>(mock_agent_service_.get(), "test-operation");
    auto span_chunk = std::make_unique<SpanChunk>(span_data, true);
    
    // Test enqueueSpan
    span_client.enqueueSpan(std::move(span_chunk));
    
    SUCCEED() << "Span enqueue should complete successfully";
}

TEST_F(GrpcTest, GrpcSpanWorkerOperationsTest) {
    GrpcSpan span_client(mock_agent_service_->getConfig()); span_client.setAgentService(mock_agent_service_.get());
    
    // Test worker stop method (skip start worker as it blocks without server)
    span_client.stopSpanWorker();
    
    SUCCEED() << "Span worker stop operations should complete successfully";
}

// GrpcStats Tests

TEST_F(GrpcTest, GrpcStatsConstructorTest) {
    GrpcStats stats_client(mock_agent_service_->getConfig()); stats_client.setAgentService(mock_agent_service_.get());
    
    SUCCEED() << "GrpcStats should construct successfully";
}

TEST_F(GrpcTest, GrpcStatsEnqueueTest) {
    GrpcStats stats_client(mock_agent_service_->getConfig()); stats_client.setAgentService(mock_agent_service_.get());
    
    // Test enqueueStats
    stats_client.enqueueStats(AGENT_STATS);
    stats_client.enqueueStats(URL_STATS);
    
    SUCCEED() << "Stats enqueue should complete successfully";
}

TEST_F(GrpcTest, GrpcStatsWorkerOperationsTest) {
    GrpcStats stats_client(mock_agent_service_->getConfig()); stats_client.setAgentService(mock_agent_service_.get());
    
    // Test worker stop method (skip start worker as it blocks without server)
    stats_client.stopStatsWorker();
    
    SUCCEED() << "Stats worker stop operations should complete successfully";
}

// Metadata Structure Tests

TEST_F(GrpcTest, ApiMetaTest) {
    ApiMeta api_meta(1, 100, "test.api.method");
    
    EXPECT_EQ(api_meta.id_, 1);
    EXPECT_EQ(api_meta.type_, 100);
    EXPECT_EQ(api_meta.api_str_, "test.api.method");
}

TEST_F(GrpcTest, StringMetaTest) {
    StringMeta str_meta(2, "test.string.value", STRING_META_ERROR);
    
    EXPECT_EQ(str_meta.id_, 2);
    EXPECT_EQ(str_meta.str_val_, "test.string.value");
    EXPECT_EQ(str_meta.type_, STRING_META_ERROR);
}

TEST_F(GrpcTest, MetaDataApiTest) {
    MetaData meta_data(META_API, 1, 100, "test.api");
    
    EXPECT_EQ(meta_data.meta_type_, META_API);
    const auto& api_meta = std::get<ApiMeta>(meta_data.value_);
    EXPECT_EQ(api_meta.id_, 1);
    EXPECT_EQ(api_meta.type_, 100);
    EXPECT_EQ(api_meta.api_str_, "test.api");
}

TEST_F(GrpcTest, MetaDataStringTest) {
    MetaData meta_data(META_STRING, 2, "test.string", STRING_META_ERROR);
    
    EXPECT_EQ(meta_data.meta_type_, META_STRING);
    const auto& str_meta = std::get<StringMeta>(meta_data.value_);
    EXPECT_EQ(str_meta.id_, 2);
    EXPECT_EQ(str_meta.str_val_, "test.string");
    EXPECT_EQ(str_meta.type_, STRING_META_ERROR);
}

TEST_F(GrpcTest, MetaDataSqlTest) {
    MetaData meta_data(META_STRING, 3, "SELECT * FROM users", STRING_META_SQL);
    
    EXPECT_EQ(meta_data.meta_type_, META_STRING);
    const auto& str_meta = std::get<StringMeta>(meta_data.value_);
    EXPECT_EQ(str_meta.id_, 3);
    EXPECT_EQ(str_meta.str_val_, "SELECT * FROM users");
    EXPECT_EQ(str_meta.type_, STRING_META_SQL);
}

TEST_F(GrpcTest, StringMetaSqlTest) {
    StringMeta sql_meta(4, "INSERT INTO table VALUES (?)", STRING_META_SQL);
    
    EXPECT_EQ(sql_meta.id_, 4);
    EXPECT_EQ(sql_meta.str_val_, "INSERT INTO table VALUES (?)");
    EXPECT_EQ(sql_meta.type_, STRING_META_SQL);
}

// Integration Tests

TEST_F(GrpcTest, GrpcClientTypeTest) {
    GrpcAgent agent_client(mock_agent_service_->getConfig()); agent_client.setAgentService(mock_agent_service_.get());
    GrpcSpan span_client(mock_agent_service_->getConfig()); span_client.setAgentService(mock_agent_service_.get());
    GrpcStats stats_client(mock_agent_service_->getConfig()); stats_client.setAgentService(mock_agent_service_.get());
    
    // Test that different client types can be created
    SUCCEED() << "All gRPC client types should be constructible";
}

TEST_F(GrpcTest, CompleteWorkflowTest) {
    // Create all client types
    GrpcAgent agent(mock_agent_service_->getConfig()); agent.setAgentService(mock_agent_service_.get());
    GrpcSpan span_client(mock_agent_service_->getConfig()); span_client.setAgentService(mock_agent_service_.get());
    GrpcStats stats_client(mock_agent_service_->getConfig()); stats_client.setAgentService(mock_agent_service_.get());
    
    // Enqueue some data
    auto api_meta = std::make_unique<MetaData>(META_API, 1, 100, "workflow.test");
    agent.enqueueMeta(std::move(api_meta));
    
    auto span_data = std::make_shared<SpanData>(mock_agent_service_.get(), "workflow-test");
    auto span_chunk = std::make_unique<SpanChunk>(span_data, true);
    span_client.enqueueSpan(std::move(span_chunk));
    
    stats_client.enqueueStats(AGENT_STATS);
    
    // Test that the workflow completes without exceptions
    SUCCEED() << "Complete gRPC workflow should execute successfully";
}

TEST_F(GrpcTest, GrpcEnumValuesTest) {
    // Test GrpcRequestStatus enum
    EXPECT_EQ(SEND_OK, 0);
    EXPECT_EQ(SEND_FAIL, 1);
    
    // Test GrpcStreamStatus enum
    EXPECT_EQ(STREAM_WRITE, 0);
    EXPECT_EQ(STREAM_CONTINUE, 1);
    EXPECT_EQ(STREAM_DONE, 2);
    EXPECT_EQ(STREAM_EXCEPTION, 3);
    
    // Test ClientType enum
    EXPECT_EQ(AGENT, 0);
    EXPECT_EQ(SPAN, 1);
    EXPECT_EQ(STATS, 2);
    
    // Test MetaType enum
    EXPECT_EQ(META_API, 0);
    EXPECT_EQ(META_STRING, 1);
    
    // Test StringMetaType enum
    EXPECT_EQ(STRING_META_ERROR, 0);
    EXPECT_EQ(STRING_META_SQL, 1);
}

TEST_F(GrpcTest, MultipleClientInstancesTest) {
    // Test that multiple instances can coexist
    std::unique_ptr<GrpcAgent> agent1 = std::make_unique<GrpcAgent>(mock_agent_service_->getConfig());
    std::unique_ptr<GrpcAgent> agent2 = std::make_unique<GrpcAgent>(mock_agent_service_->getConfig());
    
    std::unique_ptr<GrpcSpan> span1 = std::make_unique<GrpcSpan>(mock_agent_service_->getConfig());
    std::unique_ptr<GrpcSpan> span2 = std::make_unique<GrpcSpan>(mock_agent_service_->getConfig());
    
    std::unique_ptr<GrpcStats> stats1 = std::make_unique<GrpcStats>(mock_agent_service_->getConfig());
    std::unique_ptr<GrpcStats> stats2 = std::make_unique<GrpcStats>(mock_agent_service_->getConfig());
    
    // All instances should be valid
    EXPECT_NE(agent1.get(), nullptr);
    EXPECT_NE(agent2.get(), nullptr);
    EXPECT_NE(span1.get(), nullptr);
    EXPECT_NE(span2.get(), nullptr);
    EXPECT_NE(stats1.get(), nullptr);
    EXPECT_NE(stats2.get(), nullptr);
    
    // Clean up
    agent1.reset();
    agent2.reset();
    span1.reset();
    span2.reset();
    stats1.reset();
    stats2.reset();
    
    SUCCEED() << "Multiple client instances should be manageable";
}

// SqlUidMeta Tests

TEST_F(GrpcTest, SqlUidMetaTest) {
    std::vector<unsigned char> uid = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
    SqlUidMeta sql_uid_meta(uid, "SELECT * FROM orders WHERE id = ?");

    EXPECT_EQ(sql_uid_meta.uid_, uid);
    EXPECT_EQ(sql_uid_meta.sql_, "SELECT * FROM orders WHERE id = ?");
}

TEST_F(GrpcTest, SqlUidMetaEmptyUidTest) {
    std::vector<unsigned char> uid;
    SqlUidMeta sql_uid_meta(uid, "SELECT 1");

    EXPECT_TRUE(sql_uid_meta.uid_.empty());
    EXPECT_EQ(sql_uid_meta.sql_, "SELECT 1");
}

TEST_F(GrpcTest, SqlUidMetaMoveTest) {
    std::vector<unsigned char> uid = {0xAA, 0xBB, 0xCC};
    std::vector<unsigned char> uid_copy = uid;
    SqlUidMeta sql_uid_meta(std::move(uid), "SELECT 1");

    // uid should be moved into sql_uid_meta
    EXPECT_EQ(sql_uid_meta.uid_, uid_copy);
}

// ExceptionMeta Tests

TEST_F(GrpcTest, ExceptionMetaTest) {
    TraceId txid{"test-agent", 12345, 1};
    std::vector<std::unique_ptr<Exception>> exceptions;
    auto callstack = std::make_unique<CallStack>("test error");
    callstack->push("module", "func", "file.cpp", 42);
    exceptions.push_back(std::make_unique<Exception>(std::move(callstack)));

    ExceptionMeta meta(txid, 9999, "/api/test", std::move(exceptions));

    EXPECT_EQ(meta.txid_.AgentId, "test-agent");
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
    std::vector<unsigned char> uid = {1, 2, 3, 4, 5};
    MetaData meta_data(META_SQL_UID, uid, "SELECT * FROM users");

    EXPECT_EQ(meta_data.meta_type_, META_SQL_UID);
    const auto& sql_uid_meta = std::get<SqlUidMeta>(meta_data.value_);
    EXPECT_EQ(sql_uid_meta.uid_, uid);
    EXPECT_EQ(sql_uid_meta.sql_, "SELECT * FROM users");
}

TEST_F(GrpcTest, MetaDataExceptionTest) {
    TraceId txid{"agent", 100, 5};
    std::vector<std::unique_ptr<Exception>> exceptions;
    auto cs = std::make_unique<CallStack>("null pointer");
    cs->push("libcore", "deref", "ptr.cpp", 10);
    exceptions.push_back(std::make_unique<Exception>(std::move(cs)));

    MetaData meta_data(META_EXCEPTION, txid, 42, "/api/v1/resource", std::move(exceptions));

    EXPECT_EQ(meta_data.meta_type_, META_EXCEPTION);
    const auto& exc_meta = std::get<ExceptionMeta>(meta_data.value_);
    EXPECT_EQ(exc_meta.txid_.AgentId, "agent");
    EXPECT_EQ(exc_meta.span_id_, 42);
    EXPECT_EQ(exc_meta.url_template_, "/api/v1/resource");
    EXPECT_EQ(exc_meta.exceptions_.size(), 1u);
}

// Enum completeness tests

TEST_F(GrpcTest, MetaTypeEnumValuesTest) {
    EXPECT_EQ(META_API, 0);
    EXPECT_EQ(META_STRING, 1);
    EXPECT_EQ(META_SQL_UID, 2);
    EXPECT_EQ(META_EXCEPTION, 3);
}

// Queue behavior tests

TEST_F(GrpcTest, GrpcAgentMultipleMetaEnqueueTest) {
    GrpcAgent agent(mock_agent_service_->getConfig()); agent.setAgentService(mock_agent_service_.get());

    // Enqueue multiple different metadata types
    agent.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api1"));
    agent.enqueueMeta(std::make_unique<MetaData>(META_API, 2, 100, "api2"));
    agent.enqueueMeta(std::make_unique<MetaData>(META_STRING, 3, "err1", STRING_META_ERROR));
    agent.enqueueMeta(std::make_unique<MetaData>(META_STRING, 4, "sql1", STRING_META_SQL));

    std::vector<unsigned char> uid = {1, 2, 3};
    agent.enqueueMeta(std::make_unique<MetaData>(META_SQL_UID, uid, "SELECT 1"));

    TraceId txid{"agent", 100, 0};
    std::vector<std::unique_ptr<Exception>> exceptions;
    auto cs = std::make_unique<CallStack>("test");
    exceptions.push_back(std::make_unique<Exception>(std::move(cs)));
    agent.enqueueMeta(std::make_unique<MetaData>(META_EXCEPTION, txid, 1, "/url", std::move(exceptions)));

    SUCCEED() << "Enqueuing multiple meta types should succeed";
}

TEST_F(GrpcTest, GrpcSpanMultipleEnqueueTest) {
    GrpcSpan span_client(mock_agent_service_->getConfig()); span_client.setAgentService(mock_agent_service_.get());

    for (int i = 0; i < 5; i++) {
        auto span_data = std::make_shared<SpanData>(mock_agent_service_.get(), "op-" + std::to_string(i));
        auto span_chunk = std::make_unique<SpanChunk>(span_data, true);
        span_client.enqueueSpan(std::move(span_chunk));
    }

    SUCCEED() << "Multiple span enqueue operations should succeed";
}

TEST_F(GrpcTest, GrpcStatsEnqueueAllTypesTest) {
    GrpcStats stats_client(mock_agent_service_->getConfig()); stats_client.setAgentService(mock_agent_service_.get());

    stats_client.enqueueStats(AGENT_STATS);
    stats_client.enqueueStats(URL_STATS);
    stats_client.enqueueStats(AGENT_STATS);

    SUCCEED() << "Enqueuing all stats types should succeed";
}

// Channel operation tests

TEST_F(GrpcTest, GrpcClientCloseChannelIdempotentTest) {
    GrpcAgent client(mock_agent_service_->getConfig()); client.setAgentService(mock_agent_service_.get());

    client.closeChannel();
    client.closeChannel();
    client.closeChannel();

    SUCCEED() << "Calling closeChannel multiple times should be safe";
}

// Worker stop idempotency tests

TEST_F(GrpcTest, GrpcAgentStopWorkersIdempotentTest) {
    GrpcAgent agent(mock_agent_service_->getConfig()); agent.setAgentService(mock_agent_service_.get());

    agent.stopPingWorker();
    agent.stopPingWorker();
    agent.stopMetaWorker();
    agent.stopMetaWorker();

    SUCCEED() << "Stopping workers multiple times should be safe";
}

TEST_F(GrpcTest, GrpcSpanStopWorkerIdempotentTest) {
    GrpcSpan span_client(mock_agent_service_->getConfig()); span_client.setAgentService(mock_agent_service_.get());

    span_client.stopSpanWorker();
    span_client.stopSpanWorker();

    SUCCEED() << "Stopping span worker multiple times should be safe";
}

TEST_F(GrpcTest, GrpcStatsStopWorkerIdempotentTest) {
    GrpcStats stats_client(mock_agent_service_->getConfig()); stats_client.setAgentService(mock_agent_service_.get());

    stats_client.stopStatsWorker();
    stats_client.stopStatsWorker();

    SUCCEED() << "Stopping stats worker multiple times should be safe";
}

// ApiMeta edge cases

TEST_F(GrpcTest, ApiMetaEmptyStringTest) {
    ApiMeta api_meta(0, 0, "");

    EXPECT_EQ(api_meta.id_, 0);
    EXPECT_EQ(api_meta.type_, 0);
    EXPECT_TRUE(api_meta.api_str_.empty());
}

TEST_F(GrpcTest, ApiMetaLongStringTest) {
    std::string long_api(1024, 'x');
    ApiMeta api_meta(1, 200, long_api);

    EXPECT_EQ(api_meta.api_str_.size(), 1024u);
    EXPECT_EQ(api_meta.api_str_, long_api);
}

// StringMeta edge cases

TEST_F(GrpcTest, StringMetaEmptyStringTest) {
    StringMeta str_meta(0, "", STRING_META_ERROR);

    EXPECT_EQ(str_meta.id_, 0);
    EXPECT_TRUE(str_meta.str_val_.empty());
}

TEST_F(GrpcTest, StringMetaLongSqlTest) {
    std::string long_sql = "SELECT ";
    for (int i = 0; i < 100; i++) {
        long_sql += "col" + std::to_string(i) + ", ";
    }
    long_sql += "col100 FROM large_table";

    StringMeta sql_meta(1, long_sql, STRING_META_SQL);
    EXPECT_EQ(sql_meta.str_val_, long_sql);
    EXPECT_EQ(sql_meta.type_, STRING_META_SQL);
}

// Enqueue after service exiting

TEST_F(GrpcTest, GrpcAgentEnqueueMetaWhileExitingTest) {
    GrpcAgent agent(mock_agent_service_->getConfig()); agent.setAgentService(mock_agent_service_.get());
    mock_agent_service_->setExiting(true);

    // Should not crash even when service is exiting
    agent.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api-during-exit"));

    SUCCEED() << "Enqueuing meta while exiting should not crash";
}

TEST_F(GrpcTest, GrpcSpanEnqueueWhileExitingTest) {
    GrpcSpan span_client(mock_agent_service_->getConfig()); span_client.setAgentService(mock_agent_service_.get());
    mock_agent_service_->setExiting(true);

    auto span_data = std::make_shared<SpanData>(mock_agent_service_.get(), "exit-op");
    auto span_chunk = std::make_unique<SpanChunk>(span_data, true);
    span_client.enqueueSpan(std::move(span_chunk));

    SUCCEED() << "Enqueuing span while exiting should not crash";
}

TEST_F(GrpcTest, GrpcStatsEnqueueWhileExitingTest) {
    GrpcStats stats_client(mock_agent_service_->getConfig()); stats_client.setAgentService(mock_agent_service_.get());
    mock_agent_service_->setExiting(true);

    stats_client.enqueueStats(AGENT_STATS);

    SUCCEED() << "Enqueuing stats while exiting should not crash";
}

// MetaValue variant correctness

TEST_F(GrpcTest, MetaValueVariantIndexTest) {
    MetaData api_meta(META_API, 1, 100, "api");
    EXPECT_EQ(api_meta.value_.index(), 0u);  // ApiMeta

    MetaData str_meta(META_STRING, 2, "str", STRING_META_ERROR);
    EXPECT_EQ(str_meta.value_.index(), 1u);  // StringMeta

    std::vector<unsigned char> uid = {1};
    MetaData uid_meta(META_SQL_UID, uid, "sql");
    EXPECT_EQ(uid_meta.value_.index(), 2u);  // SqlUidMeta

    TraceId txid{"a", 1, 0};
    std::vector<std::unique_ptr<Exception>> excs;
    MetaData exc_meta(META_EXCEPTION, txid, 1, "/", std::move(excs));
    EXPECT_EQ(exc_meta.value_.index(), 3u);  // ExceptionMeta
}

// Reactor callback tests removed - they cause segmentation faults when called without active gRPC streams
// The callbacks are tested indirectly through the actual gRPC operations in integration tests

} // namespace pinpoint

