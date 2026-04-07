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

// Mock Writer Interface for gRPC streams
class MockClientWriter : public grpc::ClientWriterInterface<v1::PSpanMessage> {
public:
    MOCK_METHOD2(Write, bool(const v1::PSpanMessage&, grpc::WriteOptions));
    MOCK_METHOD0(WritesDone, bool());
    MOCK_METHOD0(Finish, grpc::Status());
    MOCK_METHOD1(NextMessageSize, bool(uint32_t* sz));
    MOCK_METHOD0(WaitForInitialMetadata, void());
};

class MockClientStatWriter : public grpc::ClientWriterInterface<v1::PStatMessage> {
public:
    MOCK_METHOD2(Write, bool(const v1::PStatMessage&, grpc::WriteOptions));
    MOCK_METHOD0(WritesDone, bool());
    MOCK_METHOD0(Finish, grpc::Status());
    MOCK_METHOD1(NextMessageSize, bool(uint32_t* sz));
    MOCK_METHOD0(WaitForInitialMetadata, void());
};

class MockClientReaderWriter : public grpc::ClientReaderWriterInterface<v1::PPing, v1::PPing> {
public:
    MOCK_METHOD2(Write, bool(const v1::PPing&, grpc::WriteOptions));
    MOCK_METHOD1(Read, bool(v1::PPing*));
    MOCK_METHOD0(WritesDone, bool());
    MOCK_METHOD0(Finish, grpc::Status());
    MOCK_METHOD1(NextMessageSize, bool(uint32_t* sz));
    MOCK_METHOD0(WaitForInitialMetadata, void());
};

// Testable gRPC classes that inject mock stubs
class TestableGrpcAgent : public GrpcAgent {
public:
    explicit TestableGrpcAgent(AgentService* agent) : GrpcAgent(agent->getConfig()) {
        agent_ = agent;
    }

    void setMockAgentStub(std::unique_ptr<v1::MockAgentStub> mock_stub) {
        set_agent_stub(std::move(mock_stub));
    }

    void setMockMetaStub(std::unique_ptr<v1::MockMetadataStub> mock_stub) {
        set_meta_stub(std::move(mock_stub));
    }

    // Override readyChannel — controllable per-test
    bool readyChannel() override {
        return ready_channel_;
    }

    void setReadyChannel(bool ready) { ready_channel_ = ready; }

protected:
    bool wait_channel_ready() const { return true; }

private:
    bool ready_channel_{true};
};

class TestableGrpcSpan : public GrpcSpan {
public:
    explicit TestableGrpcSpan(AgentService* agent) : GrpcSpan(agent->getConfig()) {
        agent_ = agent;
    }

    void setMockSpanStub(std::unique_ptr<v1::MockSpanStub> mock_stub) {
        set_span_stub(std::move(mock_stub));
    }

    bool readyChannel() override { return ready_channel_; }
    void setReadyChannel(bool ready) { ready_channel_ = ready; }

protected:
    bool wait_channel_ready() const { return true; }

private:
    bool ready_channel_{true};
};

class TestableGrpcStats : public GrpcStats {
public:
    explicit TestableGrpcStats(AgentService* agent) : GrpcStats(agent->getConfig()) {
        agent_ = agent;
    }

    void setMockStatsStub(std::unique_ptr<v1::MockStatStub> mock_stub) {
        set_stats_stub(std::move(mock_stub));
    }

    bool readyChannel() override { return ready_channel_; }
    void setReadyChannel(bool ready) { ready_channel_ = ready; }

protected:
    bool wait_channel_ready() const { return true; }

private:
    bool ready_channel_{true};
};

class GrpcMockTest : public ::testing::Test {
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
        // Ensure all workers are stopped before cleanup
        if (mock_agent_service_) {
            mock_agent_service_->setExiting(true);
        }
        mock_agent_service_.reset();
    }

    std::unique_ptr<MockAgentService> mock_agent_service_;
};

// GrpcAgent Tests with Mock Stubs

TEST_F(GrpcMockTest, GrpcAgentRegisterAgentSuccessTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());
    
    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    
    // Set up expectation for successful agent registration
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(Return(grpc::Status::OK));
    
    agent.setMockAgentStub(std::move(mock_agent_stub));
    
    GrpcRequestStatus status = agent.registerAgent();
    
    EXPECT_EQ(status, SEND_OK) << "Agent registration should succeed with mock stub";
}

TEST_F(GrpcMockTest, GrpcAgentRegisterAgentFailureTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());
    
    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    
    // Set up expectation for failed agent registration
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::INTERNAL, "Test error")));
    
    agent.setMockAgentStub(std::move(mock_agent_stub));
    
    GrpcRequestStatus status = agent.registerAgent();
    
    EXPECT_EQ(status, SEND_FAIL) << "Agent registration should fail with mock stub error";
}

TEST_F(GrpcMockTest, GrpcAgentMetaDataEnqueueTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());
    
    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
    agent.setMockMetaStub(std::move(mock_meta_stub));
    
    // Test metadata enqueue operations (without worker)
    auto api_meta = std::make_unique<MetaData>(META_API, 1, 100, "test.api");
    agent.enqueueMeta(std::move(api_meta));
    
    auto str_meta = std::make_unique<MetaData>(META_STRING, 2, "test.string", STRING_META_ERROR);
    agent.enqueueMeta(std::move(str_meta));
    
    SUCCEED() << "Metadata enqueue operations should succeed with mock stub";
}

TEST_F(GrpcMockTest, GrpcAgentPingWorkerTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());
    // readyChannel=false so the worker exits immediately (async streaming
    // cannot be tested with simple mock stubs)
    agent.setReadyChannel(false);

    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();

    // readyChannel returns false, so no gRPC call is expected
    EXPECT_CALL(*mock_agent_stub, PingSessionRaw(_))
        .Times(0);

    agent.setMockAgentStub(std::move(mock_agent_stub));
    
    // Test ping worker operations
    std::thread ping_worker([&agent]() {
        agent.sendPingWorker();
    });
    
    // Give worker time to start and run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Set agent to exiting state before stopping worker
    mock_agent_service_->setExiting(true);
    
    // Stop worker
    agent.stopPingWorker();
    
    // Wait for thread to finish
    if (ping_worker.joinable()) {
        ping_worker.join();
    }
    
    SUCCEED() << "Ping worker should start/stop cleanly even without successful gRPC connection";
}

TEST_F(GrpcMockTest, GrpcAgentMetaWorkerTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());
    
    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
    
    // Set up expectations for metadata operations
    EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
        .WillOnce(Return(grpc::Status::OK));
    
    EXPECT_CALL(*mock_meta_stub, RequestStringMetaData(_, _, _))
        .WillOnce(Return(grpc::Status::OK));
    
    agent.setMockMetaStub(std::move(mock_meta_stub));
    
    // Enqueue some metadata
    auto api_meta = std::make_unique<MetaData>(META_API, 1, 100, "test.api");
    agent.enqueueMeta(std::move(api_meta));
    
    auto str_meta = std::make_unique<MetaData>(META_STRING, 2, "test.string", STRING_META_ERROR);
    agent.enqueueMeta(std::move(str_meta));
    
    // Test meta worker operations
    std::thread meta_worker([&agent]() {
        agent.sendMetaWorker();
    });
    
    // Give worker time to process queue
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Set agent to exiting state before stopping worker
    mock_agent_service_->setExiting(true);
    
    // Stop worker
    agent.stopMetaWorker();
    
    // Wait for thread to finish
    if (meta_worker.joinable()) {
        meta_worker.join();
    }
    
    SUCCEED() << "Meta worker should process queued metadata with mock stub";
}

// GrpcSpan Tests with Mock Stubs

TEST_F(GrpcMockTest, GrpcSpanEnqueueSpanTest) {
    TestableGrpcSpan span_client(mock_agent_service_.get());
    
    auto mock_span_stub = std::make_unique<NiceMock<v1::MockSpanStub>>();
    span_client.setMockSpanStub(std::move(mock_span_stub));
    
    // Create test span data and enqueue (without worker)
    auto span_data = std::make_shared<SpanData>(mock_agent_service_.get(), "test-operation");
    auto span_chunk = std::make_unique<SpanChunk>(span_data, true);
    
    span_client.enqueueSpan(std::move(span_chunk));
    
    SUCCEED() << "Span enqueue should work with mock stub";
}

TEST_F(GrpcMockTest, GrpcSpanWorkerTest) {
    TestableGrpcSpan span_client(mock_agent_service_.get());
    // readyChannel=false so the worker exits immediately (async streaming
    // cannot be tested with simple mock stubs)
    span_client.setReadyChannel(false);

    auto mock_span_stub = std::make_unique<NiceMock<v1::MockSpanStub>>();

    EXPECT_CALL(*mock_span_stub, SendSpanRaw(_, _))
        .Times(0);

    span_client.setMockSpanStub(std::move(mock_span_stub));
    
    // Create test span data and enqueue
    auto span_data = std::make_shared<SpanData>(mock_agent_service_.get(), "test-operation");
    auto span_chunk = std::make_unique<SpanChunk>(span_data, true);
    span_client.enqueueSpan(std::move(span_chunk));
    
    // Test span worker operations
    std::thread span_worker([&span_client]() {
        span_client.sendSpanWorker();
    });
    
    // Give worker time to process queue
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Set agent to exiting state before stopping worker
    mock_agent_service_->setExiting(true);
    
    // Stop worker
    span_client.stopSpanWorker();
    
    // Wait for thread to finish
    if (span_worker.joinable()) {
        span_worker.join();
    }
    
    SUCCEED() << "Span worker should start/stop cleanly and process queue even without successful gRPC connection";
}

// GrpcStats Tests with Mock Stubs

TEST_F(GrpcMockTest, GrpcStatsEnqueueStatsTest) {
    TestableGrpcStats stats_client(mock_agent_service_.get());
    
    auto mock_stats_stub = std::make_unique<NiceMock<v1::MockStatStub>>();
    stats_client.setMockStatsStub(std::move(mock_stats_stub));
    
    // Enqueue some stats (without worker)
    stats_client.enqueueStats(AGENT_STATS);
    stats_client.enqueueStats(URL_STATS);
    
    SUCCEED() << "Stats enqueue should work with mock stub";
}

TEST_F(GrpcMockTest, GrpcStatsWorkerTest) {
    TestableGrpcStats stats_client(mock_agent_service_.get());
    // readyChannel=false so the worker exits immediately (async streaming
    // cannot be tested with simple mock stubs)
    stats_client.setReadyChannel(false);

    auto mock_stats_stub = std::make_unique<NiceMock<v1::MockStatStub>>();

    EXPECT_CALL(*mock_stats_stub, SendAgentStatRaw(_, _))
        .Times(0);

    stats_client.setMockStatsStub(std::move(mock_stats_stub));
    
    // Enqueue some stats
    stats_client.enqueueStats(AGENT_STATS);
    stats_client.enqueueStats(URL_STATS);
    
    // Test stats worker operations
    std::thread stats_worker([&stats_client]() {
        stats_client.sendStatsWorker();
    });
    
    // Give worker time to process queue
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Set agent to exiting state before stopping worker
    mock_agent_service_->setExiting(true);
    
    // Stop worker
    stats_client.stopStatsWorker();
    
    // Wait for thread to finish
    if (stats_worker.joinable()) {
        stats_worker.join();
    }
    
    SUCCEED() << "Stats worker should start/stop cleanly and process queue even without successful gRPC connection";
}

// Integration Tests

TEST_F(GrpcMockTest, CompleteGrpcWorkflowWithWorkersTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());
    TestableGrpcSpan span_client(mock_agent_service_.get());
    TestableGrpcStats stats_client(mock_agent_service_.get());
    
    // Set up mock stubs
    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
    auto mock_span_stub = std::make_unique<NiceMock<v1::MockSpanStub>>();
    auto mock_stats_stub = std::make_unique<NiceMock<v1::MockStatStub>>();
    
    // Set up expectations for all operations
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(Return(grpc::Status::OK));
    
    // Ping session expectation (gRPC connection will fail)
    EXPECT_CALL(*mock_agent_stub, PingSessionRaw(_))
        .Times(0); // No calls expected due to connection failure
    
    // Metadata expectations (these might succeed if worker processes queue before connection)
    EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
        .WillRepeatedly(Return(grpc::Status::OK));
    
    // Span expectations (gRPC connection will fail)  
    EXPECT_CALL(*mock_span_stub, SendSpanRaw(_, _))
        .Times(0); // No calls expected due to connection failure
    
    // Stats expectations (gRPC connection will fail)
    EXPECT_CALL(*mock_stats_stub, SendAgentStatRaw(_, _))
        .Times(0); // No calls expected due to connection failure
    
    // Inject mocks
    agent.setMockAgentStub(std::move(mock_agent_stub));
    agent.setMockMetaStub(std::move(mock_meta_stub));
    span_client.setMockSpanStub(std::move(mock_span_stub));
    stats_client.setMockStatsStub(std::move(mock_stats_stub));
    
    // Test complete workflow
    GrpcRequestStatus register_status = agent.registerAgent();
    EXPECT_EQ(register_status, SEND_OK);

    // Disable readyChannel for workers (async streaming cannot be tested with mock stubs)
    agent.setReadyChannel(false);
    span_client.setReadyChannel(false);
    stats_client.setReadyChannel(false);

    // Start workers
    std::thread ping_worker([&agent]() { agent.sendPingWorker(); });
    std::thread meta_worker([&agent]() { agent.sendMetaWorker(); });
    std::thread span_worker([&span_client]() { span_client.sendSpanWorker(); });
    std::thread stats_worker([&stats_client]() { stats_client.sendStatsWorker(); });
    
    // Enqueue data
    auto api_meta = std::make_unique<MetaData>(META_API, 1, 100, "workflow.test");
    agent.enqueueMeta(std::move(api_meta));
    
    auto span_data = std::make_shared<SpanData>(mock_agent_service_.get(), "workflow-test");
    auto span_chunk = std::make_unique<SpanChunk>(span_data, true);
    span_client.enqueueSpan(std::move(span_chunk));
    
    stats_client.enqueueStats(AGENT_STATS);
    
    // Give workers time to process
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Set agent to exiting state before stopping workers
    mock_agent_service_->setExiting(true);
    
    // Stop all workers
    agent.stopPingWorker();
    agent.stopMetaWorker();
    span_client.stopSpanWorker();
    stats_client.stopStatsWorker();
    
    // Wait for threads to finish
    if (ping_worker.joinable()) ping_worker.join();
    if (meta_worker.joinable()) meta_worker.join();
    if (span_worker.joinable()) span_worker.join();
    if (stats_worker.joinable()) stats_worker.join();
    
    SUCCEED() << "Complete gRPC workflow with workers should start/stop cleanly and process metadata even without gRPC connections";
}

TEST_F(GrpcMockTest, AllWorkersStartStopTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());
    TestableGrpcSpan span_client(mock_agent_service_.get());
    TestableGrpcStats stats_client(mock_agent_service_.get());
    
    // Set up basic mock stubs
    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
    auto mock_span_stub = std::make_unique<NiceMock<v1::MockSpanStub>>();
    auto mock_stats_stub = std::make_unique<NiceMock<v1::MockStatStub>>();
    
    // Mock setup for ping session (gRPC connection will fail)
    EXPECT_CALL(*mock_agent_stub, PingSessionRaw(_))
        .Times(0); // No calls expected due to connection failure
    
    // Mock setup for span sending (gRPC connection will fail)
    EXPECT_CALL(*mock_span_stub, SendSpanRaw(_, _))
        .Times(0); // No calls expected due to connection failure
    
    // Mock setup for stats sending (gRPC connection will fail)
    EXPECT_CALL(*mock_stats_stub, SendAgentStatRaw(_, _))
        .Times(0); // No calls expected due to connection failure
    
    // Inject mocks
    agent.setMockAgentStub(std::move(mock_agent_stub));
    agent.setMockMetaStub(std::move(mock_meta_stub));
    span_client.setMockSpanStub(std::move(mock_span_stub));
    stats_client.setMockStatsStub(std::move(mock_stats_stub));

    // Disable readyChannel for workers (async streaming cannot be tested with mock stubs)
    agent.setReadyChannel(false);
    span_client.setReadyChannel(false);
    stats_client.setReadyChannel(false);

    // Start all workers
    std::thread ping_worker([&agent]() { agent.sendPingWorker(); });
    std::thread meta_worker([&agent]() { agent.sendMetaWorker(); });
    std::thread span_worker([&span_client]() { span_client.sendSpanWorker(); });
    std::thread stats_worker([&stats_client]() { stats_client.sendStatsWorker(); });

    // Let workers run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Set agent to exiting state before stopping workers
    mock_agent_service_->setExiting(true);
    
    // Stop all workers
    agent.stopPingWorker();
    agent.stopMetaWorker();
    span_client.stopSpanWorker();
    stats_client.stopStatsWorker();
    
    // All threads should be joinable and finish quickly
    if (ping_worker.joinable()) ping_worker.join();
    if (meta_worker.joinable()) meta_worker.join();
    if (span_worker.joinable()) span_worker.join();
    if (stats_worker.joinable()) stats_worker.join();
    
    SUCCEED() << "All workers should start and stop cleanly even without successful gRPC connections";
}

TEST_F(GrpcMockTest, MockStubVerificationTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());
    
    auto mock_agent_stub = std::make_unique<StrictMock<v1::MockAgentStub>>();
    
    // Strict mock will fail if unexpected methods are called
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .Times(1)
        .WillOnce(Return(grpc::Status::OK));
    
    agent.setMockAgentStub(std::move(mock_agent_stub));
    
    // This should call RequestAgentInfo exactly once
    GrpcRequestStatus status = agent.registerAgent();
    EXPECT_EQ(status, SEND_OK);
    
    // If we call registerAgent again, StrictMock would fail the test
    // But we won't to demonstrate that mock verification works
    
    SUCCEED() << "Mock stub verification should work correctly";
}

// ============================================================
// Metadata send failure tests
// ============================================================

TEST_F(GrpcMockTest, GrpcAgentSendApiMetaFailureTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();

    // API meta send fails
    EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::UNAVAILABLE, "server unavailable")));

    agent.setMockMetaStub(std::move(mock_meta_stub));

    // Enqueue API meta and run worker
    agent.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "fail.api"));

    std::thread meta_worker([&agent]() { agent.sendMetaWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    SUCCEED() << "API meta failure should be handled gracefully";
}

TEST_F(GrpcMockTest, GrpcAgentSendErrorMetaFailureTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();

    EXPECT_CALL(*mock_meta_stub, RequestStringMetaData(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::INTERNAL, "internal error")));

    agent.setMockMetaStub(std::move(mock_meta_stub));

    agent.enqueueMeta(std::make_unique<MetaData>(META_STRING, 1, "error msg", STRING_META_ERROR));

    std::thread meta_worker([&agent]() { agent.sendMetaWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    SUCCEED() << "Error meta failure should be handled gracefully";
}

TEST_F(GrpcMockTest, GrpcAgentSendSqlMetaFailureTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();

    EXPECT_CALL(*mock_meta_stub, RequestSqlMetaData(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "timeout")));

    agent.setMockMetaStub(std::move(mock_meta_stub));

    agent.enqueueMeta(std::make_unique<MetaData>(META_STRING, 1, "SELECT 1", STRING_META_SQL));

    std::thread meta_worker([&agent]() { agent.sendMetaWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    SUCCEED() << "SQL meta failure should be handled gracefully";
}

TEST_F(GrpcMockTest, GrpcAgentSendSqlUidMetaFailureTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();

    EXPECT_CALL(*mock_meta_stub, RequestSqlUidMetaData(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "denied")));

    agent.setMockMetaStub(std::move(mock_meta_stub));

    std::vector<unsigned char> uid = {1, 2, 3};
    agent.enqueueMeta(std::make_unique<MetaData>(META_SQL_UID, uid, "SELECT * FROM t"));

    std::thread meta_worker([&agent]() { agent.sendMetaWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    SUCCEED() << "SQL UID meta failure should be handled gracefully";
}

TEST_F(GrpcMockTest, GrpcAgentSendExceptionMetaFailureTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();

    EXPECT_CALL(*mock_meta_stub, RequestExceptionMetaData(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "resource exhausted")));

    agent.setMockMetaStub(std::move(mock_meta_stub));

    TraceId txid{"agent", 100, 0};
    std::vector<std::unique_ptr<Exception>> exceptions;
    auto cs = std::make_unique<CallStack>("test error");
    cs->push("mod", "func", "file.cpp", 10);
    exceptions.push_back(std::make_unique<Exception>(std::move(cs)));
    agent.enqueueMeta(std::make_unique<MetaData>(META_EXCEPTION, txid, 1, "/api", std::move(exceptions)));

    std::thread meta_worker([&agent]() { agent.sendMetaWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    SUCCEED() << "Exception meta failure should be handled gracefully";
}

// ============================================================
// Mixed metadata success/failure in a single worker run
// ============================================================

TEST_F(GrpcMockTest, GrpcAgentMetaWorkerMixedSuccessFailureTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();

    {
        InSequence seq;
        // First API meta succeeds
        EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
            .WillOnce(Return(grpc::Status::OK));
        // Second API meta fails
        EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
            .WillOnce(Return(grpc::Status(grpc::StatusCode::UNAVAILABLE, "unavailable")));
        // Third API meta succeeds (recovery after failure)
        EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
            .WillOnce(Return(grpc::Status::OK));
    }

    agent.setMockMetaStub(std::move(mock_meta_stub));

    agent.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api.ok"));
    agent.enqueueMeta(std::make_unique<MetaData>(META_API, 2, 100, "api.fail"));
    agent.enqueueMeta(std::make_unique<MetaData>(META_API, 3, 100, "api.recover"));

    std::thread meta_worker([&agent]() { agent.sendMetaWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    SUCCEED() << "Worker should continue processing after a failure";
}

// ============================================================
// Agent registration with various gRPC error codes
// ============================================================

TEST_F(GrpcMockTest, GrpcAgentRegisterUnavailableTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();

    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::UNAVAILABLE, "service unavailable")));

    agent.setMockAgentStub(std::move(mock_agent_stub));

    EXPECT_EQ(agent.registerAgent(), SEND_FAIL);
}

TEST_F(GrpcMockTest, GrpcAgentRegisterDeadlineExceededTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();

    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "deadline exceeded")));

    agent.setMockAgentStub(std::move(mock_agent_stub));

    EXPECT_EQ(agent.registerAgent(), SEND_FAIL);
}

TEST_F(GrpcMockTest, GrpcAgentRegisterPermissionDeniedTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();

    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "permission denied")));

    agent.setMockAgentStub(std::move(mock_agent_stub));

    EXPECT_EQ(agent.registerAgent(), SEND_FAIL);
}

TEST_F(GrpcMockTest, GrpcAgentRegisterRetrySuccessTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();

    // First call fails, second succeeds
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::UNAVAILABLE, "temporary failure")))
        .WillOnce(Return(grpc::Status::OK));

    agent.setMockAgentStub(std::move(mock_agent_stub));

    EXPECT_EQ(agent.registerAgent(), SEND_FAIL);
    EXPECT_EQ(agent.registerAgent(), SEND_OK);
}

// ============================================================
// All metadata types sent successfully via worker
// ============================================================

TEST_F(GrpcMockTest, GrpcAgentMetaWorkerAllTypesSuccessTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();

    EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
        .WillOnce(Return(grpc::Status::OK));
    EXPECT_CALL(*mock_meta_stub, RequestStringMetaData(_, _, _))
        .WillOnce(Return(grpc::Status::OK));
    EXPECT_CALL(*mock_meta_stub, RequestSqlMetaData(_, _, _))
        .WillOnce(Return(grpc::Status::OK));
    EXPECT_CALL(*mock_meta_stub, RequestSqlUidMetaData(_, _, _))
        .WillOnce(Return(grpc::Status::OK));
    EXPECT_CALL(*mock_meta_stub, RequestExceptionMetaData(_, _, _))
        .WillOnce(Return(grpc::Status::OK));

    agent.setMockMetaStub(std::move(mock_meta_stub));

    // Enqueue all metadata types
    agent.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "test.api"));
    agent.enqueueMeta(std::make_unique<MetaData>(META_STRING, 2, "error msg", STRING_META_ERROR));
    agent.enqueueMeta(std::make_unique<MetaData>(META_STRING, 3, "SELECT 1", STRING_META_SQL));

    std::vector<unsigned char> uid = {1, 2, 3};
    agent.enqueueMeta(std::make_unique<MetaData>(META_SQL_UID, uid, "SELECT * FROM t"));

    TraceId txid{"agent", 100, 0};
    std::vector<std::unique_ptr<Exception>> exceptions;
    auto cs = std::make_unique<CallStack>("err");
    exceptions.push_back(std::make_unique<Exception>(std::move(cs)));
    agent.enqueueMeta(std::make_unique<MetaData>(META_EXCEPTION, txid, 1, "/api", std::move(exceptions)));

    std::thread meta_worker([&agent]() { agent.sendMetaWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    SUCCEED() << "All metadata types should be sent successfully";
}

// ============================================================
// All metadata types fail via worker
// ============================================================

TEST_F(GrpcMockTest, GrpcAgentMetaWorkerAllTypesFailureTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();

    EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::INTERNAL, "fail")));
    EXPECT_CALL(*mock_meta_stub, RequestStringMetaData(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::INTERNAL, "fail")));
    EXPECT_CALL(*mock_meta_stub, RequestSqlMetaData(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::INTERNAL, "fail")));
    EXPECT_CALL(*mock_meta_stub, RequestSqlUidMetaData(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::INTERNAL, "fail")));
    EXPECT_CALL(*mock_meta_stub, RequestExceptionMetaData(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::INTERNAL, "fail")));

    agent.setMockMetaStub(std::move(mock_meta_stub));

    agent.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "test.api"));
    agent.enqueueMeta(std::make_unique<MetaData>(META_STRING, 2, "err", STRING_META_ERROR));
    agent.enqueueMeta(std::make_unique<MetaData>(META_STRING, 3, "SELECT 1", STRING_META_SQL));

    std::vector<unsigned char> uid = {1, 2, 3};
    agent.enqueueMeta(std::make_unique<MetaData>(META_SQL_UID, uid, "SELECT * FROM t"));

    TraceId txid{"agent", 100, 0};
    std::vector<std::unique_ptr<Exception>> exceptions;
    auto cs = std::make_unique<CallStack>("err");
    exceptions.push_back(std::make_unique<Exception>(std::move(cs)));
    agent.enqueueMeta(std::make_unique<MetaData>(META_EXCEPTION, txid, 1, "/api", std::move(exceptions)));

    std::thread meta_worker([&agent]() { agent.sendMetaWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    SUCCEED() << "Worker should handle all metadata type failures gracefully";
}

// ============================================================
// Stats worker disabled when stat config is off
// ============================================================

TEST_F(GrpcMockTest, GrpcStatsWorkerDisabledWhenStatAndUrlStatDisabledTest) {
    // Disable both stat and url_stat
    auto cfg = std::make_shared<Config>();
    *cfg = *mock_agent_service_->getConfig();
    cfg->stat.enable = false;
    cfg->http.url_stat.enable = false;
    mock_agent_service_->reloadConfig(cfg);

    TestableGrpcStats stats_client(mock_agent_service_.get());

    auto mock_stats_stub = std::make_unique<StrictMock<v1::MockStatStub>>();
    // StrictMock: no calls expected since stats is disabled
    stats_client.setMockStatsStub(std::move(mock_stats_stub));

    // enqueueStats should be a no-op when disabled
    stats_client.enqueueStats(AGENT_STATS);
    stats_client.enqueueStats(URL_STATS);

    // sendStatsWorker should return immediately when disabled
    std::thread stats_worker([&stats_client]() { stats_client.sendStatsWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_agent_service_->setExiting(true);
    stats_client.stopStatsWorker();

    if (stats_worker.joinable()) stats_worker.join();

    SUCCEED() << "Stats worker should be no-op when both stat and url_stat are disabled";
}

// ============================================================
// SQL meta and SQL UID meta success tests
// ============================================================

TEST_F(GrpcMockTest, GrpcAgentSendSqlMetaSuccessTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();

    EXPECT_CALL(*mock_meta_stub, RequestSqlMetaData(_, _, _))
        .WillOnce(Return(grpc::Status::OK));

    agent.setMockMetaStub(std::move(mock_meta_stub));

    agent.enqueueMeta(std::make_unique<MetaData>(META_STRING, 1, "SELECT * FROM users", STRING_META_SQL));

    std::thread meta_worker([&agent]() { agent.sendMetaWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    SUCCEED() << "SQL meta should be sent successfully";
}

TEST_F(GrpcMockTest, GrpcAgentSendSqlUidMetaSuccessTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();

    EXPECT_CALL(*mock_meta_stub, RequestSqlUidMetaData(_, _, _))
        .WillOnce(Return(grpc::Status::OK));

    agent.setMockMetaStub(std::move(mock_meta_stub));

    std::vector<unsigned char> uid = {0xAA, 0xBB, 0xCC, 0xDD};
    agent.enqueueMeta(std::make_unique<MetaData>(META_SQL_UID, uid, "INSERT INTO t VALUES (?)"));

    std::thread meta_worker([&agent]() { agent.sendMetaWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    SUCCEED() << "SQL UID meta should be sent successfully";
}

TEST_F(GrpcMockTest, GrpcAgentSendExceptionMetaSuccessTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();

    EXPECT_CALL(*mock_meta_stub, RequestExceptionMetaData(_, _, _))
        .WillOnce(Return(grpc::Status::OK));

    agent.setMockMetaStub(std::move(mock_meta_stub));

    TraceId txid{"test-agent", 12345, 1};
    std::vector<std::unique_ptr<Exception>> exceptions;
    auto cs = std::make_unique<CallStack>("NullPointerException");
    cs->push("libcore", "deref", "ptr.cpp", 42);
    cs->push("app", "main", "main.cpp", 100);
    exceptions.push_back(std::make_unique<Exception>(std::move(cs)));

    agent.enqueueMeta(std::make_unique<MetaData>(META_EXCEPTION, txid, 999, "/api/v2/resource", std::move(exceptions)));

    std::thread meta_worker([&agent]() { agent.sendMetaWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    SUCCEED() << "Exception meta should be sent successfully";
}

// ============================================================
// Multiple registrations (success then failure)
// ============================================================

TEST_F(GrpcMockTest, GrpcAgentMultipleRegisterTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();

    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(Return(grpc::Status::OK))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "already registered")))
        .WillOnce(Return(grpc::Status::OK));

    agent.setMockAgentStub(std::move(mock_agent_stub));

    EXPECT_EQ(agent.registerAgent(), SEND_OK);
    EXPECT_EQ(agent.registerAgent(), SEND_FAIL);
    EXPECT_EQ(agent.registerAgent(), SEND_OK);
}

} // namespace pinpoint

