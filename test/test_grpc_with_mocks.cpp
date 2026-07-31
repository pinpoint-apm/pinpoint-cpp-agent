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
#include <functional>
#include <future>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../src/grpc.h"
#include "../src/agent_service.h"
#include "../src/config.h"
#include "../src/span.h"
#include "../src/stat.h"
#include "../src/url_stat.h"
#include "../include/pinpoint/tracer.h"
#include "v1/Service_mock.grpc.pb.h"
#include "mock_agent_service.h"
#include "mock_helpers.h"

using ::testing::_;
using ::testing::Return;
using ::testing::NiceMock;
using ::testing::StrictMock;
using ::testing::InSequence;
using ::testing::DoAll;
using ::testing::SetArgPointee;
using ::testing::SaveArg;
using ::testing::Invoke;
using ::testing::InvokeWithoutArgs;

namespace pinpoint {

static v1::PResult success_result() {
    v1::PResult result;
    result.set_success(true);
    return result;
}

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

// Mock for the HandleCommandV2 bidirectional command stream
class MockCmdStream : public grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest> {
public:
    MOCK_METHOD2(Write, bool(const v1::PCmdMessage&, grpc::WriteOptions));
    MOCK_METHOD1(Read, bool(v1::PCmdRequest*));
    MOCK_METHOD0(WritesDone, bool());
    MOCK_METHOD0(Finish, grpc::Status());
    MOCK_METHOD1(NextMessageSize, bool(uint32_t* sz));
    MOCK_METHOD0(WaitForInitialMetadata, void());
};

// Mock for the CommandStreamActiveThreadCount client-streaming writer
class MockActiveThreadCountWriter : public grpc::ClientWriterInterface<v1::PCmdActiveThreadCountRes> {
public:
    MOCK_METHOD2(Write, bool(const v1::PCmdActiveThreadCountRes&, grpc::WriteOptions));
    MOCK_METHOD0(WritesDone, bool());
    MOCK_METHOD0(Finish, grpc::Status());
    MOCK_METHOD1(NextMessageSize, bool(uint32_t* sz));
    MOCK_METHOD0(WaitForInitialMetadata, void());
};

// Hand-written fake for the Span stub: the generated MockSpanStub cannot serve
// the callback-based async()->SendSpanBatch() path used by GrpcSpan (its
// async() returns nullptr), so this fake implements async_interface and lets
// tests capture requests and control when each RPC's completion callback runs.
class FakeSpanStub : public v1::Span::StubInterface {
public:
    enum class ReplyMode {
        OK_EMPTY,
        OK_PARTIAL_SUCCESS,
        ERROR_STATUS,
        HOLD,
        THROW_BEFORE_CALLBACK
    };

    FakeSpanStub() : fake_async_(this) {}

    grpc::Status SendSpanBatch(grpc::ClientContext*, const v1::PSpanMessageBatch&,
                               v1::PSpanResultBatch*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "sync SendSpanBatch unused");
    }

    async_interface* async() override { return &fake_async_; }

    void setReplyMode(ReplyMode mode) {
        std::unique_lock<std::mutex> lock(mutex_);
        mode_ = mode;
    }

    size_t batchCount() {
        std::unique_lock<std::mutex> lock(mutex_);
        return requests_.size();
    }

    v1::PSpanMessageBatch request(size_t index) {
        std::unique_lock<std::mutex> lock(mutex_);
        return requests_.at(index);
    }

    bool waitForBatchCount(size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return requests_.size() >= count; });
    }

    void releaseHeldCallbacks(const grpc::Status& status) {
        std::vector<std::function<void(grpc::Status)>> held;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            held.swap(held_);
        }
        for (auto& callback : held) {
            callback(status);
        }
    }

    bool releaseHeldCallback(size_t index, const grpc::Status& status) {
        std::function<void(grpc::Status)> held;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (index >= held_.size()) {
                return false;
            }
            held = std::move(held_[index]);
            held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(index));
        }
        held(status);
        return true;
    }

private:
    class FakeAsync : public v1::Span::StubInterface::async_interface {
    public:
        explicit FakeAsync(FakeSpanStub* owner) : owner_(owner) {}
        void SendSpan(grpc::ClientContext*, google::protobuf::Empty*,
                      grpc::ClientWriteReactor<v1::PSpanMessage>*) override {}
        void SendSpanBatch(grpc::ClientContext*, const v1::PSpanMessageBatch* request,
                           v1::PSpanResultBatch* response,
                           std::function<void(grpc::Status)> on_done) override {
            owner_->handleSendSpanBatch(request, response, std::move(on_done));
        }
        void SendSpanBatch(grpc::ClientContext*, const v1::PSpanMessageBatch*,
                           v1::PSpanResultBatch*, grpc::ClientUnaryReactor*) override {}

    private:
        FakeSpanStub* owner_;
    };

    void handleSendSpanBatch(const v1::PSpanMessageBatch* request, v1::PSpanResultBatch* response,
                             std::function<void(grpc::Status)> on_done) {
        std::function<void(grpc::Status)> to_invoke;
        grpc::Status status = grpc::Status::OK;
        bool throw_before_callback = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            requests_.push_back(*request);
            switch (mode_) {
                case ReplyMode::OK_EMPTY:
                    to_invoke = std::move(on_done);
                    break;
                case ReplyMode::OK_PARTIAL_SUCCESS: {
                    auto* partial = response->mutable_partial_success();
                    partial->set_rejected_spans(1);
                    partial->set_errorid(7);
                    partial->set_error_message("rejected by fake");
                    to_invoke = std::move(on_done);
                    break;
                }
                case ReplyMode::ERROR_STATUS:
                    status = grpc::Status(grpc::StatusCode::UNAVAILABLE, "fake unavailable");
                    to_invoke = std::move(on_done);
                    break;
                case ReplyMode::HOLD:
                    held_.push_back(std::move(on_done));
                    break;
                case ReplyMode::THROW_BEFORE_CALLBACK:
                    throw_before_callback = true;
                    break;
            }
        }
        cv_.notify_all();
        if (throw_before_callback) {
            throw std::runtime_error("fake async SendSpanBatch launch failure");
        }
        if (to_invoke) {
            to_invoke(status);
        }
    }

    grpc::ClientWriterInterface<v1::PSpanMessage>* SendSpanRaw(
        grpc::ClientContext*, google::protobuf::Empty*) override { return nullptr; }
    grpc::ClientAsyncWriterInterface<v1::PSpanMessage>* AsyncSendSpanRaw(
        grpc::ClientContext*, google::protobuf::Empty*, grpc::CompletionQueue*, void*) override { return nullptr; }
    grpc::ClientAsyncWriterInterface<v1::PSpanMessage>* PrepareAsyncSendSpanRaw(
        grpc::ClientContext*, google::protobuf::Empty*, grpc::CompletionQueue*) override { return nullptr; }
    grpc::ClientAsyncResponseReaderInterface<v1::PSpanResultBatch>* AsyncSendSpanBatchRaw(
        grpc::ClientContext*, const v1::PSpanMessageBatch&, grpc::CompletionQueue*) override { return nullptr; }
    grpc::ClientAsyncResponseReaderInterface<v1::PSpanResultBatch>* PrepareAsyncSendSpanBatchRaw(
        grpc::ClientContext*, const v1::PSpanMessageBatch&, grpc::CompletionQueue*) override { return nullptr; }

    FakeAsync fake_async_;
    std::mutex mutex_;
    std::condition_variable cv_;
    ReplyMode mode_{ReplyMode::OK_EMPTY};
    std::vector<v1::PSpanMessageBatch> requests_;
    std::vector<std::function<void(grpc::Status)>> held_;
};

// Testable gRPC classes that inject mock stubs
class TestableGrpcMetadata : public GrpcMetadata {
public:
    explicit TestableGrpcMetadata(AgentService* agent, const GrpcClientTuning& tuning = {})
        : GrpcMetadata(agent->getConfig(), tuning) {
        setAgentService(agent);
    }

    void setMockMetaStub(std::unique_ptr<v1::MockMetadataStub> mock_stub) {
        set_meta_stub(std::move(mock_stub));
    }

    bool readyChannel() override {
        if (ready_channel_failures_.load() > 0) {
            ready_channel_failures_.fetch_sub(1);
            return false;
        }
        return ready_channel_;
    }

    void setReadyChannel(bool ready) { ready_channel_ = ready; }

    // Models a transient collector outage: the next N readiness checks fail
    // (channel down), after which ready_channel_ applies again (recovered).
    void setReadyChannelFailures(int failures) { ready_channel_failures_ = failures; }

    // Retry-specific tests shrink these so retries fire in milliseconds; the
    // defaults match production so other tests never see an in-test retry.
    // Must be called before the worker thread starts.
    void setRetryDelay(std::chrono::milliseconds delay) { tuning_.meta_retry_delay = delay; }
    void setRetryMaxAttempts(int attempts) { tuning_.meta_retry_max_attempts = attempts; }

private:
    // Toggled by the test body while the worker thread polls readyChannel();
    // atomic so TSan-clean, like ready_channel_failures_.
    std::atomic<bool> ready_channel_{true};
    std::atomic<int> ready_channel_failures_{0};
};

class TestableGrpcAgent : public GrpcAgent {
public:
    explicit TestableGrpcAgent(AgentService* agent, const GrpcClientTuning& tuning = {})
        : GrpcAgent(agent->getConfig(), tuning), metadata_(agent, tuning) {
        setAgentService(agent);
    }

    void setMockAgentStub(std::unique_ptr<v1::MockAgentStub> mock_stub) {
        set_agent_stub(std::move(mock_stub));
    }

    void setMockMetaStub(std::unique_ptr<v1::MockMetadataStub> mock_stub) {
        metadata_.setMockMetaStub(std::move(mock_stub));
    }

    void enqueueMeta(std::unique_ptr<MetaData> meta) noexcept {
        metadata_.enqueueMeta(std::move(meta));
    }

    void sendMetaWorker() {
        metadata_.sendMetaWorker();
    }

    void stopMetaWorker() {
        metadata_.stopMetaWorker();
    }

    // Override readyChannel — controllable per-test
    bool readyChannel() override {
        return ready_channel_;
    }

    void setReadyChannel(bool ready) {
        ready_channel_ = ready;
        metadata_.setReadyChannel(ready);
    }

protected:
    bool wait_channel_ready() const { return true; }

private:
    // Toggled by the test body while the worker thread polls readyChannel();
    // atomic so TSan-clean, like ready_channel_failures_.
    std::atomic<bool> ready_channel_{true};
    TestableGrpcMetadata metadata_;
};

class RetryingAgentInfoGrpcAgent : public GrpcAgent {
public:
    explicit RetryingAgentInfoGrpcAgent(std::shared_ptr<const Config> config)
        : GrpcAgent(std::move(config)) {}

    GrpcRequestStatus registerAgent() override {
        return calls_.fetch_add(1) + 1 >= 2 ? SEND_OK : SEND_FAIL;
    }

    int calls() const {
        return calls_.load();
    }

private:
    std::atomic<int> calls_{0};
};

class TestableGrpcSpan : public GrpcSpan {
public:
    explicit TestableGrpcSpan(AgentService* agent, const GrpcClientTuning& tuning = {})
        : GrpcSpan(agent->getConfig(), tuning) {
        agent_ = agent;
    }

    void setMockSpanStub(std::unique_ptr<v1::Span::StubInterface> mock_stub) {
        set_span_stub(std::move(mock_stub));
    }

    bool readyChannel() override { return ready_channel_; }
    void setReadyChannel(bool ready) { ready_channel_ = ready; }

protected:
    bool wait_channel_ready() const { return true; }

private:
    // Toggled by the test body while the worker thread polls readyChannel();
    // atomic so TSan-clean, like ready_channel_failures_.
    std::atomic<bool> ready_channel_{true};
};

class TestableGrpcStats : public GrpcStats {
public:
    explicit TestableGrpcStats(AgentService* agent, const GrpcClientTuning& tuning = {})
        : GrpcStats(agent->getConfig(), tuning) {
        agent_ = agent;
    }

    void setMockStatsStub(std::unique_ptr<v1::MockStatStub> mock_stub) {
        set_stats_stub(std::move(mock_stub));
    }

    bool readyChannel() override { return ready_channel_; }
    void setReadyChannel(bool ready) { ready_channel_ = ready; }
    void markSlowChannelRecoveryForTest() { on_slow_channel_recovery(std::chrono::seconds(5)); }
    bool emptyStatsQueueIfRequestedForTest() { return empty_stats_queue_if_requested(); }

protected:
    bool wait_channel_ready() const { return true; }

private:
    // Toggled by the test body while the worker thread polls readyChannel();
    // atomic so TSan-clean, like ready_channel_failures_.
    std::atomic<bool> ready_channel_{true};
};

class ThrowingReadyGrpcAgent : public GrpcAgent {
public:
    explicit ThrowingReadyGrpcAgent(std::shared_ptr<const Config> config,
                                    const GrpcClientTuning& tuning = {})
        : GrpcAgent(std::move(config), tuning) {}

    bool readyChannel() override {
        ++attempts_;
        throw std::runtime_error("injected ping channel setup failure");
    }

    int attempts() const { return attempts_.load(); }

private:
    std::atomic<int> attempts_{0};
};

class ThrowingReadyGrpcStats : public GrpcStats {
public:
    explicit ThrowingReadyGrpcStats(std::shared_ptr<const Config> config)
        : GrpcStats(std::move(config)) {}

    bool readyChannel() override {
        throw std::runtime_error("injected stats channel setup failure");
    }
};

// Models a persistent collector outage: readyChannel() never succeeds but
// records each attempt, so tests can observe the workers' retry cadence.
class CountingNotReadyGrpcAgent : public GrpcAgent {
public:
    explicit CountingNotReadyGrpcAgent(std::shared_ptr<const Config> config,
                                       const GrpcClientTuning& tuning = {})
        : GrpcAgent(std::move(config), tuning) {}

    bool readyChannel() override {
        ++ready_attempts_;
        return false;
    }

    int readyAttempts() const { return ready_attempts_.load(); }

private:
    std::atomic<int> ready_attempts_{0};
};

class CountingNotReadyGrpcStats : public GrpcStats {
public:
    explicit CountingNotReadyGrpcStats(std::shared_ptr<const Config> config,
                                       const GrpcClientTuning& tuning = {})
        : GrpcStats(std::move(config), tuning) {}

    bool readyChannel() override {
        ++ready_attempts_;
        return false;
    }

    int readyAttempts() const { return ready_attempts_.load(); }

private:
    std::atomic<int> ready_attempts_{0};
};

class TestableGrpcCommand : public GrpcCommand {
public:
    explicit TestableGrpcCommand(AgentService* agent, const GrpcClientTuning& tuning = {})
        : GrpcCommand(agent->getConfig(), tuning) {
        setAgentService(agent);
    }

    void setMockCommandStub(std::unique_ptr<v1::ProfilerCommandService::StubInterface> mock_stub) {
        set_command_stub(std::move(mock_stub));
    }

    bool readyChannel() override { return ready_channel_; }
    void setReadyChannel(bool ready) { ready_channel_ = ready; }

private:
    // Toggled by the test body while the worker thread polls readyChannel();
    // atomic so TSan-clean, like ready_channel_failures_.
    std::atomic<bool> ready_channel_{true};
};

// GrpcAgent whose registerAgent() throws on the first attempt, for the
// boot-registration and AgentInfo-scheduler supervised-retry tests.
class ThrowingAgentInfoGrpcAgent : public GrpcAgent {
public:
    explicit ThrowingAgentInfoGrpcAgent(std::shared_ptr<const Config> config)
        : GrpcAgent(std::move(config)) {}

    GrpcRequestStatus registerAgent() override {
        if (calls_.fetch_add(1) == 0) {
            throw std::runtime_error("injected AgentInfo build failure");
        }
        if (success_promise_ != nullptr && !promise_set_.exchange(true)) {
            success_promise_->set_value();
        }
        return SEND_OK;
    }

    void setSuccessPromise(std::promise<void>* promise) {
        success_promise_ = promise;
    }

    int calls() const {
        return calls_.load();
    }

private:
    std::atomic<int> calls_{0};
    std::atomic<bool> promise_set_{false};
    std::promise<void>* success_promise_{nullptr};
};

// GrpcAgent whose registerAgent() is a counting stub, for AgentInfo scheduler tests
class CountingAgentInfoGrpcAgent : public GrpcAgent {
public:
    CountingAgentInfoGrpcAgent(std::shared_ptr<const Config> config, GrpcRequestStatus result)
        : GrpcAgent(std::move(config)), result_(result) {}

    GrpcRequestStatus registerAgent() override {
        ++calls_;
        return result_;
    }

    int calls() const { return calls_.load(); }

private:
    std::atomic<int> calls_{0};
    GrpcRequestStatus result_;
};

static bool wait_for_condition(const std::function<bool()>& condition, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
}

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

TEST_F(GrpcMockTest, GrpcAgentRegisterAgentUsesDefaultServerMetaData) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    v1::PAgentInfo captured_agent_info;
    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(DoAll(SaveArg<1>(&captured_agent_info), Return(grpc::Status::OK)));

    agent.setMockAgentStub(std::move(mock_agent_stub));

    EXPECT_EQ(agent.registerAgent(), SEND_OK);

    ASSERT_TRUE(captured_agent_info.has_servermetadata());
    const auto& server_metadata = captured_agent_info.servermetadata();
    EXPECT_EQ(server_metadata.serverinfo(), "C/C++ Application");
    EXPECT_EQ(server_metadata.vmarg_size(), 0);
    ASSERT_EQ(server_metadata.serviceinfo_size(), 1);

    const auto& service_info = server_metadata.serviceinfo(0);
    EXPECT_EQ(service_info.servicename(), "Pinpoint Agent");

    auto has_service_lib = [&service_info](const std::string& expected) {
        for (const auto& service_lib : service_info.servicelib()) {
            if (service_lib == expected) {
                return true;
            }
        }
        return false;
    };

    EXPECT_TRUE(has_service_lib("Span.MaxEventDepth=32"));
    EXPECT_TRUE(has_service_lib("Span.EventChunkSize=10"));
    EXPECT_TRUE(has_service_lib("Http.CollectUrlStat=true"));
    EXPECT_TRUE(has_service_lib("Http.UrlStatTrimPathDepth=3"));
}

TEST_F(GrpcMockTest, GrpcAgentRegisterAgentUsesServerMetaData) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    v1::PAgentInfo captured_agent_info;
    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(DoAll(SaveArg<1>(&captured_agent_info), Return(grpc::Status::OK)));

    agent.setMockAgentStub(std::move(mock_agent_stub));
    agent.setServerMetaData("test-server", {"--port=8080", "--worker=4"}, {"libfoo.so", "libbar.so"});

    EXPECT_EQ(agent.registerAgent(), SEND_OK);

    ASSERT_TRUE(captured_agent_info.has_servermetadata());
    const auto& server_metadata = captured_agent_info.servermetadata();
    EXPECT_EQ(server_metadata.serverinfo(), "test-server");
    ASSERT_EQ(server_metadata.vmarg_size(), 2);
    EXPECT_EQ(server_metadata.vmarg(0), "--port=8080");
    EXPECT_EQ(server_metadata.vmarg(1), "--worker=4");

    ASSERT_EQ(server_metadata.serviceinfo_size(), 2);
    const auto& service_info = server_metadata.serviceinfo(0);
    EXPECT_EQ(service_info.servicename(), "Libraries");
    ASSERT_EQ(service_info.servicelib_size(), 2);
    EXPECT_EQ(service_info.servicelib(0), "libfoo.so");
    EXPECT_EQ(service_info.servicelib(1), "libbar.so");

    const auto& config_service_info = server_metadata.serviceinfo(1);
    EXPECT_EQ(config_service_info.servicename(), "Pinpoint Agent");
    auto has_config_service_lib = [&config_service_info](const std::string& expected) {
        for (const auto& service_lib : config_service_info.servicelib()) {
            if (service_lib == expected) {
                return true;
            }
        }
        return false;
    };
    EXPECT_TRUE(has_config_service_lib("Span.MaxEventDepth=32"));
    EXPECT_TRUE(has_config_service_lib("Http.CollectUrlStat=true"));
}

TEST_F(GrpcMockTest, GrpcAgentRegisterAgentFailureTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();

    // registerAgent maps every non-OK status to SEND_FAIL regardless of code
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::INTERNAL, "internal error")))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::UNAVAILABLE, "service unavailable")))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "deadline exceeded")))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "permission denied")));

    agent.setMockAgentStub(std::move(mock_agent_stub));

    EXPECT_EQ(agent.registerAgent(), SEND_FAIL);
    EXPECT_EQ(agent.registerAgent(), SEND_FAIL);
    EXPECT_EQ(agent.registerAgent(), SEND_FAIL);
    EXPECT_EQ(agent.registerAgent(), SEND_FAIL);
}

TEST_F(GrpcMockTest, GrpcAgentRegisterWithRetryRetriesUntilSuccess) {
    auto cfg = mock_agent_service_->mutableConfig();
    cfg->collector.agent_info.send_retry_interval_ms = 10;

    RetryingAgentInfoGrpcAgent grpc_agent(cfg);
    grpc_agent.setAgentService(mock_agent_service_.get());

    EXPECT_TRUE(grpc_agent.registerAgentWithRetry())
        << "boot registration should retry until the collector accepts AgentInfo";
    EXPECT_GE(grpc_agent.calls(), 2);
}

TEST_F(GrpcMockTest, GrpcAgentRegisterWithRetrySurvivesRegisterException) {
    auto cfg = mock_agent_service_->mutableConfig();
    cfg->collector.agent_info.send_retry_interval_ms = 10;

    ThrowingAgentInfoGrpcAgent grpc_agent(cfg);
    grpc_agent.setAgentService(mock_agent_service_.get());

    // The first attempt throws. The boot loop must treat it like a failed
    // send and keep retrying instead of aborting the bring-up.
    EXPECT_TRUE(grpc_agent.registerAgentWithRetry())
        << "boot registration should survive a thrown registerAgent()";
    EXPECT_GE(grpc_agent.calls(), 2);
}

TEST_F(GrpcMockTest, GrpcAgentRegisterWithRetryStopsWhenAgentExits) {
    auto cfg = mock_agent_service_->mutableConfig();
    cfg->collector.agent_info.send_retry_interval_ms = 10;

    CountingAgentInfoGrpcAgent grpc_agent(cfg, SEND_FAIL);
    grpc_agent.setAgentService(mock_agent_service_.get());

    mock_agent_service_->setExiting(true);
    EXPECT_FALSE(grpc_agent.registerAgentWithRetry())
        << "boot registration must give up when the agent is exiting";
    EXPECT_EQ(grpc_agent.calls(), 0);
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

TEST_F(GrpcMockTest, GrpcAgentPingWorkerContainsChannelSetupException) {
    ThrowingReadyGrpcAgent agent(mock_agent_service_->getConfig());
    agent.setAgentService(mock_agent_service_.get());

    // The worker must contain the exception and keep retrying (supervised
    // restart) instead of dying — so it only returns once stopped.
    std::thread ping_worker([&agent]() {
        EXPECT_NO_THROW(agent.sendPingWorker());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    agent.stopPingWorker();
    ping_worker.join();
    EXPECT_FALSE(mock_agent_service_->isExiting());
}

TEST_F(GrpcMockTest, GrpcAgentMetaWorkerTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());
    
    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
    
    // Set up expectations for metadata operations
    EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
        .WillOnce(DoAll(SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));
    
    EXPECT_CALL(*mock_meta_stub, RequestStringMetaData(_, _, _))
        .WillOnce(DoAll(SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));
    
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
    auto span_data = make_test_span_data_ptr(*mock_agent_service_, "test-operation");
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
    auto span_data = make_test_span_data_ptr(*mock_agent_service_, "test-operation");
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

TEST_F(GrpcMockTest, GrpcStatsOverflowRequestsStatsQueuePurge) {
    TestableGrpcStats stats_client(mock_agent_service_.get());

    mock_agent_service_->getAgentStats().incrSampleNew();
    UrlStatEntry url_stat{"/overflow", "GET", 200};
    url_stat.end_time_ = std::chrono::system_clock::now();
    url_stat.elapsed_ = 10;
    mock_agent_service_->getUrlStats().addSnapshot(&url_stat, *mock_agent_service_->getConfig());

    stats_client.enqueueStats(AGENT_STATS);
    stats_client.enqueueStats(URL_STATS);
    stats_client.enqueueStats(AGENT_STATS);

    EXPECT_TRUE(stats_client.emptyStatsQueueIfRequestedForTest());
    EXPECT_FALSE(stats_client.emptyStatsQueueIfRequestedForTest());

    AgentStatsSnapshot agent_snapshot;
    mock_agent_service_->getAgentStats().collectAgentStat(agent_snapshot);
    EXPECT_EQ(agent_snapshot.num_sample_new_, 0);
    EXPECT_TRUE(mock_agent_service_->getUrlStats().takeSnapshot()->getEachStats().empty());
}

TEST_F(GrpcMockTest, GrpcStatsSlowChannelRecoveryRequestsStatsQueuePurge) {
    TestableGrpcStats stats_client(mock_agent_service_.get());

    mock_agent_service_->getAgentStats().incrSampleNew();
    UrlStatEntry url_stat{"/reconnect", "GET", 200};
    url_stat.end_time_ = std::chrono::system_clock::now();
    url_stat.elapsed_ = 10;
    mock_agent_service_->getUrlStats().addSnapshot(&url_stat, *mock_agent_service_->getConfig());

    stats_client.enqueueStats(AGENT_STATS);
    stats_client.markSlowChannelRecoveryForTest();

    EXPECT_TRUE(stats_client.emptyStatsQueueIfRequestedForTest());
    EXPECT_FALSE(stats_client.emptyStatsQueueIfRequestedForTest());

    AgentStatsSnapshot agent_snapshot;
    mock_agent_service_->getAgentStats().collectAgentStat(agent_snapshot);
    EXPECT_EQ(agent_snapshot.num_sample_new_, 0);
    EXPECT_TRUE(mock_agent_service_->getUrlStats().takeSnapshot()->getEachStats().empty());
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

TEST_F(GrpcMockTest, GrpcStatsWorkerContainsChannelSetupException) {
    ThrowingReadyGrpcStats stats_client(mock_agent_service_->getConfig());
    stats_client.setAgentService(mock_agent_service_.get());

    // The worker must contain the exception and keep retrying (supervised
    // restart) instead of dying — so it only returns once stopped.
    std::thread stats_worker([&stats_client]() {
        EXPECT_NO_THROW(stats_client.sendStatsWorker());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stats_client.stopStatsWorker();
    stats_worker.join();
    EXPECT_FALSE(mock_agent_service_->isExiting());
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
        .WillRepeatedly(DoAll(SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));
    
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
    
    auto span_data = make_test_span_data_ptr(*mock_agent_service_, "workflow-test");
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

    SqlUid uid = {1, 2, 3};
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
            .WillOnce(DoAll(SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));
        // Second API meta fails
        EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
            .WillOnce(Return(grpc::Status(grpc::StatusCode::UNAVAILABLE, "unavailable")));
        // Third API meta succeeds (recovery after failure)
        EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
            .WillOnce(DoAll(SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));
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

TEST_F(GrpcMockTest, GrpcMetadataRetriesFailedResultWithoutEvictingCache) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(std::chrono::milliseconds(50));

    v1::PResult failed;
    failed.set_success(false);

    std::atomic<int> attempts{0};
    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
    EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
        .WillOnce(DoAll(InvokeWithoutArgs([&attempts] { ++attempts; }),
                        SetArgPointee<2>(failed), Return(grpc::Status::OK)))
        .WillOnce(DoAll(InvokeWithoutArgs([&attempts] { ++attempts; }),
                        SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));

    metadata.setMockMetaStub(std::move(mock_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api.retry"));

    std::thread meta_worker([&metadata]() { metadata.sendMetaWorker(); });

    // PResult.success=false must be retried after the (shrunk) retry delay
    EXPECT_TRUE(wait_for_condition([&attempts] { return attempts.load() >= 2; }, std::chrono::seconds(5)));

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(mock_agent_service_->removed_api_count_, 0);
}

TEST_F(GrpcMockTest, GrpcMetadataRetriesItemWhenSendThrows) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(std::chrono::milliseconds(50));

    std::atomic<int> attempts{0};
    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
    EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
        .WillOnce(Invoke([&attempts](grpc::ClientContext*,
                                    const v1::PApiMetaData&,
                                    v1::PResult*) -> grpc::Status {
            ++attempts;
            throw std::runtime_error("transient metadata send failure");
        }))
        .WillOnce(DoAll(InvokeWithoutArgs([&attempts] { ++attempts; }),
                        SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));

    metadata.setMockMetaStub(std::move(mock_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api.throw.retry"));

    std::thread meta_worker([&metadata]() { metadata.sendMetaWorker(); });

    EXPECT_TRUE(wait_for_condition(
        [&attempts] { return attempts.load() >= 2; }, std::chrono::seconds(5)));

    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(attempts.load(), 2);
    EXPECT_EQ(mock_agent_service_->removed_api_count_, 0);
}

TEST_F(GrpcMockTest, GrpcMetadataSkipsRpcWhenChannelNotReady) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setReadyChannel(false);
    metadata.setRetryDelay(std::chrono::milliseconds(50));

    auto mock_meta_stub = std::make_unique<StrictMock<v1::MockMetadataStub>>();
    EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _)).Times(0);

    metadata.setMockMetaStub(std::move(mock_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api.not.ready"));

    std::thread meta_worker([&metadata]() { metadata.sendMetaWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();
}

TEST_F(GrpcMockTest, GrpcMetadataEvictsCacheAfterRetryExhaustion) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(std::chrono::milliseconds(50));

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
    EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
        .Times(4)
        .WillRepeatedly(Return(grpc::Status(grpc::StatusCode::UNAVAILABLE, "unavailable")));

    metadata.setMockMetaStub(std::move(mock_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api.exhaust"));

    std::thread meta_worker([&metadata]() { metadata.sendMetaWorker(); });

    // Eviction happens only after 3 scheduled retries are exhausted
    EXPECT_TRUE(wait_for_condition(
        [this] { return mock_agent_service_->removed_api_count_ >= 1; }, std::chrono::seconds(10)));

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(mock_agent_service_->removed_api_count_, 1);
}

TEST_F(GrpcMockTest, GrpcMetadataEvictsErrorCacheAfterRetryExhaustion) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(std::chrono::milliseconds(50));

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
    EXPECT_CALL(*mock_meta_stub, RequestStringMetaData(_, _, _))
        .Times(4)
        .WillRepeatedly(Return(
            grpc::Status(grpc::StatusCode::UNAVAILABLE, "unavailable")));

    metadata.setMockMetaStub(std::move(mock_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(
        META_STRING, 2, "error.exhaust", STRING_META_ERROR));

    std::thread meta_worker([&metadata]() { metadata.sendMetaWorker(); });

    EXPECT_TRUE(wait_for_condition(
        [this] { return mock_agent_service_->removed_error_count_.load() >= 1; },
        std::chrono::seconds(10)));

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(mock_agent_service_->removed_error_count_.load(), 1);
}

TEST_F(GrpcMockTest, GrpcMetadataEvictsSqlCacheAfterRetryExhaustion) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(std::chrono::milliseconds(50));

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
    EXPECT_CALL(*mock_meta_stub, RequestSqlMetaData(_, _, _))
        .Times(4)
        .WillRepeatedly(Return(
            grpc::Status(grpc::StatusCode::UNAVAILABLE, "unavailable")));

    metadata.setMockMetaStub(std::move(mock_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(
        META_STRING, 3, "SELECT exhaust", STRING_META_SQL));

    std::thread meta_worker([&metadata]() { metadata.sendMetaWorker(); });

    EXPECT_TRUE(wait_for_condition(
        [this] { return mock_agent_service_->removed_sql_count_.load() >= 1; },
        std::chrono::seconds(10)));

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(mock_agent_service_->removed_sql_count_.load(), 1);
}

TEST_F(GrpcMockTest, GrpcMetadataEvictsSqlUidCacheAfterRetryExhaustion) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(std::chrono::milliseconds(50));

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
    EXPECT_CALL(*mock_meta_stub, RequestSqlUidMetaData(_, _, _))
        .Times(4)
        .WillRepeatedly(Return(
            grpc::Status(grpc::StatusCode::UNAVAILABLE, "unavailable")));

    metadata.setMockMetaStub(std::move(mock_meta_stub));
    const SqlUid uid{0, 1, 2, 3, 4, 5, 6, 7,
                     8, 9, 10, 11, 12, 13, 14, 15};
    metadata.enqueueMeta(std::make_unique<MetaData>(
        META_SQL_UID, uid, "SELECT uid_exhaust"));

    std::thread meta_worker([&metadata]() { metadata.sendMetaWorker(); });

    EXPECT_TRUE(wait_for_condition(
        [this] { return mock_agent_service_->removed_sql_uid_count_.load() >= 1; },
        std::chrono::seconds(10)));

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(mock_agent_service_->removed_sql_uid_count_.load(), 1);
}

// ============================================================
// All metadata types sent successfully via worker
// ============================================================

TEST_F(GrpcMockTest, GrpcAgentMetaWorkerAllTypesSuccessTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();

    EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
        .WillOnce(DoAll(SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));
    EXPECT_CALL(*mock_meta_stub, RequestStringMetaData(_, _, _))
        .WillOnce(DoAll(SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));
    EXPECT_CALL(*mock_meta_stub, RequestSqlMetaData(_, _, _))
        .WillOnce(DoAll(SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));
    EXPECT_CALL(*mock_meta_stub, RequestSqlUidMetaData(_, _, _))
        .WillOnce(DoAll(SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));
    EXPECT_CALL(*mock_meta_stub, RequestExceptionMetaData(_, _, _))
        .WillOnce(DoAll(SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));

    agent.setMockMetaStub(std::move(mock_meta_stub));

    // Enqueue all metadata types
    agent.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "test.api"));
    agent.enqueueMeta(std::make_unique<MetaData>(META_STRING, 2, "error msg", STRING_META_ERROR));
    agent.enqueueMeta(std::make_unique<MetaData>(META_STRING, 3, "SELECT 1", STRING_META_SQL));

    SqlUid uid = {1, 2, 3};
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

    SqlUid uid = {1, 2, 3};
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
        .WillOnce(DoAll(SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));

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
        .WillOnce(DoAll(SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));

    agent.setMockMetaStub(std::move(mock_meta_stub));

    SqlUid uid = {0xAA, 0xBB, 0xCC, 0xDD};
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
        .WillOnce(DoAll(SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));

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

// ============================================================
// GrpcCommandDispatcher unit tests
// ============================================================

TEST(GrpcCommandDispatcherTest, RoutesRequestToRegisteredHandler) {
    GrpcCommandDispatcher dispatcher;

    v1::PCmdRequest received;
    dispatcher.registerHandler(static_cast<int32_t>(v1::ECHO),
        [&received](const v1::PCmdRequest& request,
                    grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>*) {
            received = request;
            return true;
        });

    v1::PCmdRequest request;
    request.set_requestid(7);
    request.mutable_commandecho()->set_message("ping");

    EXPECT_TRUE(dispatcher.handle(request, nullptr));
    EXPECT_EQ(received.requestid(), 7);
    EXPECT_EQ(received.commandecho().message(), "ping");
}

TEST(GrpcCommandDispatcherTest, HandlerReturnValuePropagates) {
    GrpcCommandDispatcher dispatcher;

    dispatcher.registerHandler(static_cast<int32_t>(v1::ECHO),
        [](const v1::PCmdRequest&,
           grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>*) {
            return false;
        });

    v1::PCmdRequest request;
    request.set_requestid(1);
    request.mutable_commandecho()->set_message("ping");

    EXPECT_FALSE(dispatcher.handle(request, nullptr));
}

TEST(GrpcCommandDispatcherTest, UnknownCommandWritesNotSupportedFailMessage) {
    GrpcCommandDispatcher dispatcher;

    v1::PCmdRequest request;
    request.set_requestid(42);

    StrictMock<MockCmdStream> stream;
    v1::PCmdMessage fail_message;
    EXPECT_CALL(stream, Write(_, _))
        .WillOnce(DoAll(SaveArg<0>(&fail_message), Return(true)));

    EXPECT_TRUE(dispatcher.handle(request, &stream));
    ASSERT_TRUE(fail_message.has_failmessage());
    EXPECT_EQ(fail_message.failmessage().responseid(), 42);
    EXPECT_EQ(fail_message.failmessage().message().value(), "NOT_SUPPORTED_REQUEST");
}

TEST(GrpcCommandDispatcherTest, UnknownCommandFailWriteFailureReturnsFalse) {
    GrpcCommandDispatcher dispatcher;

    v1::PCmdRequest request;
    request.set_requestid(43);

    StrictMock<MockCmdStream> stream;
    EXPECT_CALL(stream, Write(_, _)).WillOnce(Return(false));

    EXPECT_FALSE(dispatcher.handle(request, &stream));
}

TEST(GrpcCommandDispatcherTest, UnknownCommandWithNullStreamReturnsTrue) {
    GrpcCommandDispatcher dispatcher;

    v1::PCmdRequest request;
    request.set_requestid(44);

    EXPECT_TRUE(dispatcher.handle(request, nullptr));
}

TEST(GrpcCommandDispatcherTest, SupportedCommandCodesAreSorted) {
    GrpcCommandDispatcher dispatcher;

    auto noop = [](const v1::PCmdRequest&,
                   grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>*) {
        return true;
    };
    dispatcher.registerHandler(static_cast<int32_t>(v1::ACTIVE_THREAD_COUNT), noop);
    dispatcher.registerHandler(static_cast<int32_t>(v1::ECHO), noop);

    const auto codes = dispatcher.supportedCommandCodes();
    ASSERT_EQ(codes.size(), 2u);
    EXPECT_EQ(codes[0], static_cast<int32_t>(v1::ECHO));
    EXPECT_EQ(codes[1], static_cast<int32_t>(v1::ACTIVE_THREAD_COUNT));
}

// ============================================================
// GrpcCommand worker tests
// ============================================================

namespace {
    // Fallback streams for reconnect iterations: Read immediately reports
    // end-of-stream so extra HandleCommandV2 connections terminate fast.
    grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>* make_idle_cmd_stream(grpc::ClientContext*) {
        return new NiceMock<MockCmdStream>();
    }
}

TEST_F(GrpcMockTest, GrpcCommandWorkerEchoTest) {
    TestableGrpcCommand command(mock_agent_service_.get());

    auto mock_command_stub = std::make_unique<NiceMock<v1::MockProfilerCommandServiceStub>>();

    v1::PCmdRequest echo_request;
    echo_request.set_requestid(99);
    echo_request.mutable_commandecho()->set_message("hello");

    auto* stream = new NiceMock<MockCmdStream>();
    EXPECT_CALL(*stream, Read(_))
        .WillOnce(DoAll(SetArgPointee<0>(echo_request), Return(true)))
        .WillRepeatedly(Return(false));

    EXPECT_CALL(*mock_command_stub, HandleCommandV2Raw(_))
        .WillOnce(Return(stream))
        .WillRepeatedly(Invoke(make_idle_cmd_stream));

    v1::PCmdEchoResponse echo_response;
    std::promise<void> echo_seen;
    EXPECT_CALL(*mock_command_stub, CommandEcho(_, _, _))
        .WillOnce(DoAll(SaveArg<1>(&echo_response),
                        InvokeWithoutArgs([&echo_seen] { echo_seen.set_value(); }),
                        Return(grpc::Status::OK)));

    command.setMockCommandStub(std::move(mock_command_stub));

    std::thread worker([&command] { command.commandWorker(); });

    EXPECT_EQ(echo_seen.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "Echo command should be relayed to CommandEcho RPC";

    mock_agent_service_->setExiting(true);
    command.stopCommandWorker();
    if (worker.joinable()) worker.join();

    EXPECT_EQ(echo_response.commonresponse().responseid(), 99);
    EXPECT_EQ(echo_response.message(), "hello");
}

TEST_F(GrpcMockTest, GrpcCommandWorkerEchoFailureWritesFailMessage) {
    TestableGrpcCommand command(mock_agent_service_.get());

    auto mock_command_stub = std::make_unique<NiceMock<v1::MockProfilerCommandServiceStub>>();

    v1::PCmdRequest echo_request;
    echo_request.set_requestid(55);
    echo_request.mutable_commandecho()->set_message("hello");

    auto* stream = new NiceMock<MockCmdStream>();
    EXPECT_CALL(*stream, Read(_))
        .WillOnce(DoAll(SetArgPointee<0>(echo_request), Return(true)))
        .WillRepeatedly(Return(false));

    v1::PCmdMessage fail_message;
    std::promise<void> fail_seen;
    EXPECT_CALL(*stream, Write(_, _))
        .WillOnce(DoAll(SaveArg<0>(&fail_message),
                        InvokeWithoutArgs([&fail_seen] { fail_seen.set_value(); }),
                        Return(true)));

    EXPECT_CALL(*mock_command_stub, HandleCommandV2Raw(_))
        .WillOnce(Return(stream))
        .WillRepeatedly(Invoke(make_idle_cmd_stream));

    EXPECT_CALL(*mock_command_stub, CommandEcho(_, _, _))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::UNAVAILABLE, "echo backend down")));

    command.setMockCommandStub(std::move(mock_command_stub));

    std::thread worker([&command] { command.commandWorker(); });

    EXPECT_EQ(fail_seen.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "CommandEcho failure should produce a fail message on the command stream";

    mock_agent_service_->setExiting(true);
    command.stopCommandWorker();
    if (worker.joinable()) worker.join();

    ASSERT_TRUE(fail_message.has_failmessage());
    EXPECT_EQ(fail_message.failmessage().responseid(), 55);
    EXPECT_EQ(fail_message.failmessage().message().value(), "echo backend down");
}

TEST_F(GrpcMockTest, GrpcCommandWorkerUnknownCommandWritesFailMessage) {
    TestableGrpcCommand command(mock_agent_service_.get());

    auto mock_command_stub = std::make_unique<NiceMock<v1::MockProfilerCommandServiceStub>>();

    v1::PCmdRequest unknown_request;
    unknown_request.set_requestid(11);  // no command payload set

    auto* stream = new NiceMock<MockCmdStream>();
    EXPECT_CALL(*stream, Read(_))
        .WillOnce(DoAll(SetArgPointee<0>(unknown_request), Return(true)))
        .WillRepeatedly(Return(false));

    v1::PCmdMessage fail_message;
    std::promise<void> fail_seen;
    EXPECT_CALL(*stream, Write(_, _))
        .WillOnce(DoAll(SaveArg<0>(&fail_message),
                        InvokeWithoutArgs([&fail_seen] { fail_seen.set_value(); }),
                        Return(true)));

    EXPECT_CALL(*mock_command_stub, HandleCommandV2Raw(_))
        .WillOnce(Return(stream))
        .WillRepeatedly(Invoke(make_idle_cmd_stream));

    command.setMockCommandStub(std::move(mock_command_stub));

    std::thread worker([&command] { command.commandWorker(); });

    EXPECT_EQ(fail_seen.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "Unknown command should produce a NOT_SUPPORTED_REQUEST fail message";

    mock_agent_service_->setExiting(true);
    command.stopCommandWorker();
    if (worker.joinable()) worker.join();

    ASSERT_TRUE(fail_message.has_failmessage());
    EXPECT_EQ(fail_message.failmessage().responseid(), 11);
    EXPECT_EQ(fail_message.failmessage().message().value(), "NOT_SUPPORTED_REQUEST");
}

TEST_F(GrpcMockTest, GrpcCommandWorkerActiveThreadCountTest) {
    TestableGrpcCommand command(mock_agent_service_.get());

    auto mock_command_stub = std::make_unique<NiceMock<v1::MockProfilerCommandServiceStub>>();

    v1::PCmdRequest atc_request;
    atc_request.set_requestid(77);
    atc_request.mutable_commandactivethreadcount();

    auto* stream = new NiceMock<MockCmdStream>();
    EXPECT_CALL(*stream, Read(_))
        .WillOnce(DoAll(SetArgPointee<0>(atc_request), Return(true)))
        .WillRepeatedly(Return(false));

    EXPECT_CALL(*mock_command_stub, HandleCommandV2Raw(_))
        .WillOnce(Return(stream))
        .WillRepeatedly(Invoke(make_idle_cmd_stream));

    // Returning false from Write ends the active thread count stream after
    // the first response so the test does not wait on the 1s flush delay.
    auto* atc_writer = new NiceMock<MockActiveThreadCountWriter>();
    v1::PCmdActiveThreadCountRes atc_response;
    std::promise<void> atc_seen;
    EXPECT_CALL(*atc_writer, Write(_, _))
        .WillOnce(DoAll(SaveArg<0>(&atc_response),
                        InvokeWithoutArgs([&atc_seen] { atc_seen.set_value(); }),
                        Return(false)));

    EXPECT_CALL(*mock_command_stub, CommandStreamActiveThreadCountRaw(_, _))
        .WillOnce(Return(atc_writer));

    command.setMockCommandStub(std::move(mock_command_stub));

    std::thread worker([&command] { command.commandWorker(); });

    EXPECT_EQ(atc_seen.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "Active thread count command should start a response stream";

    mock_agent_service_->setExiting(true);
    command.stopCommandWorker();
    if (worker.joinable()) worker.join();

    EXPECT_EQ(atc_response.commonstreamresponse().responseid(), 77);
    EXPECT_EQ(atc_response.commonstreamresponse().sequenceid(), 1);
    EXPECT_EQ(atc_response.histogramschematype(), 2);
    EXPECT_EQ(atc_response.activethreadcount_size(), 4);
}

TEST_F(GrpcMockTest, GrpcCommandWorkerExitsWhenChannelNotReady) {
    TestableGrpcCommand command(mock_agent_service_.get());
    command.setReadyChannel(false);

    // StrictMock: no RPC may be attempted when the channel never becomes ready
    auto mock_command_stub = std::make_unique<StrictMock<v1::MockProfilerCommandServiceStub>>();
    command.setMockCommandStub(std::move(mock_command_stub));

    std::thread worker([&command] { command.commandWorker(); });
    if (worker.joinable()) worker.join();

    SUCCEED() << "Command worker should exit immediately when the channel is not ready";
}

TEST_F(GrpcMockTest, GrpcCommandStopWorkerWakesReconnectDelay) {
    TestableGrpcCommand command(mock_agent_service_.get());

    auto mock_command_stub = std::make_unique<NiceMock<v1::MockProfilerCommandServiceStub>>();

    std::promise<void> stream_opened;
    std::atomic<bool> opened_once{false};
    EXPECT_CALL(*mock_command_stub, HandleCommandV2Raw(_))
        .WillRepeatedly(Invoke([&](grpc::ClientContext* ctx) {
            if (!opened_once.exchange(true)) {
                stream_opened.set_value();
            }
            return make_idle_cmd_stream(ctx);
        }));

    command.setMockCommandStub(std::move(mock_command_stub));

    std::thread worker([&command] { command.commandWorker(); });

    ASSERT_EQ(stream_opened.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);

    // The worker is now in (or heading into) the 1s reconnect delay;
    // stopCommandWorker must wake it so shutdown does not block.
    mock_agent_service_->setExiting(true);
    command.stopCommandWorker();
    if (worker.joinable()) worker.join();

    SUCCEED() << "stopCommandWorker should wake the reconnect delay and end the worker";
}

// ============================================================
// GrpcSpan SendSpanBatch tests (fake async stub)
// ============================================================

TEST_F(GrpcMockTest, GrpcSpanSendBatchSuccessTest) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_batch.size = 2;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 100;
    cfg->collector.span_batch.max_concurrent_requests = 2;

    TestableGrpcSpan span_client(mock_agent_service_.get());
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    span_client.setMockSpanStub(std::move(fake_stub));

    auto span_data1 = make_test_span_data_ptr(*mock_agent_service_, "batch-op-1");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data1, true));
    auto span_data2 = make_test_span_data_ptr(*mock_agent_service_, "batch-op-2");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data2, true));

    std::thread worker([&span_client] { span_client.sendSpanWorker(); });

    ASSERT_TRUE(fake->waitForBatchCount(1, std::chrono::seconds(2)))
        << "A batch should be sent via async SendSpanBatch";

    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();

    const auto request = fake->request(0);
    ASSERT_EQ(request.span_size(), 2);
    EXPECT_TRUE(request.span(0).has_span()) << "Final non-async chunk should be encoded as PSpan";
    EXPECT_TRUE(request.span(1).has_span());
}

TEST_F(GrpcMockTest, GrpcSpanSendBatchSpanVsSpanChunkTest) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_batch.size = 2;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 100;
    cfg->collector.span_batch.max_concurrent_requests = 2;

    TestableGrpcSpan span_client(mock_agent_service_.get());
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    span_client.setMockSpanStub(std::move(fake_stub));

    auto final_span = make_test_span_data_ptr(*mock_agent_service_, "final-op");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(final_span, true));
    auto partial_span = make_test_span_data_ptr(*mock_agent_service_, "partial-op");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(partial_span, false));

    std::thread worker([&span_client] { span_client.sendSpanWorker(); });

    ASSERT_TRUE(fake->waitForBatchCount(1, std::chrono::seconds(2)));

    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();

    const auto request = fake->request(0);
    ASSERT_EQ(request.span_size(), 2);
    EXPECT_TRUE(request.span(0).has_span()) << "Final chunk should be encoded as PSpan";
    EXPECT_TRUE(request.span(1).has_spanchunk()) << "Non-final chunk should be encoded as PSpanChunk";
}

TEST_F(GrpcMockTest, GrpcSpanBatchCarriesParentServiceNameTest) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_batch.size = 1;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 100;
    cfg->collector.span_batch.max_concurrent_requests = 2;

    TestableGrpcSpan span_client(mock_agent_service_.get());
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    span_client.setMockSpanStub(std::move(fake_stub));

    auto span_data = make_test_span_data_ptr(*mock_agent_service_, "parent-service-op");
    // parentinfo is only emitted when the parent application name is present.
    span_data->setParentAppName("ParentApp");
    span_data->setParentServiceName("parent-service");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data, true));

    std::thread worker([&span_client] { span_client.sendSpanWorker(); });

    ASSERT_TRUE(fake->waitForBatchCount(1, std::chrono::seconds(2)));

    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();

    const auto request = fake->request(0);
    ASSERT_EQ(request.span_size(), 1);
    ASSERT_TRUE(request.span(0).has_span());
    const auto& parent_info = request.span(0).span().acceptevent().parentinfo();
    EXPECT_EQ(parent_info.parentservicename(), "parent-service")
        << "Built gRPC span should carry parentServiceName (PParentInfo field 4)";
}

TEST_F(GrpcMockTest, GrpcSpanBatchSerializesAnnotationsFromVariantValueTest) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_batch.size = 1;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 100;
    cfg->collector.span_batch.max_concurrent_requests = 2;

    TestableGrpcSpan span_client(mock_agent_service_.get());
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    span_client.setMockSpanStub(std::move(fake_stub));

    SqlUid uid{};
    uid[0] = 0xDE;
    uid[1] = 0xAD;
    uid[2] = 0xBE;
    uid[3] = 0xEF;

    auto span_data = make_test_span_data_ptr(*mock_agent_service_, "annotation-op");
    auto& annotations = span_data->getAnnotations()->getAnnotations();
    annotations.emplace_back(101, AnnotationData(int32_t{42}));
    span_data->getAnnotations()->AppendLong(102, 1234567890123LL);
    span_data->getAnnotations()->AppendString(103, "string-value");
    span_data->getAnnotations()->AppendStringString(104, "left", "right");
    span_data->getAnnotations()->AppendIntStringString(105, 7, "method", "GET");
    span_data->getAnnotations()->AppendLongIntIntByteByteString(106, 99, 1, 2, 3, 4, "rpc");
    span_data->getAnnotations()->AppendSqlUidStringString(107, uid, "sql", "args");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data, true));

    std::thread worker([&span_client] { span_client.sendSpanWorker(); });

    ASSERT_TRUE(fake->waitForBatchCount(1, std::chrono::seconds(2)));

    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();

    const auto request = fake->request(0);
    ASSERT_EQ(request.span_size(), 1);
    ASSERT_TRUE(request.span(0).has_span());
    const auto& span = request.span(0).span();
    ASSERT_EQ(span.annotation_size(), 7);

    EXPECT_EQ(span.annotation(0).key(), 101);
    EXPECT_EQ(span.annotation(0).value().intvalue(), 42)
        << "An int annotation should serialize as intvalue";

    EXPECT_EQ(span.annotation(1).key(), 102);
    EXPECT_EQ(span.annotation(1).value().longvalue(), 1234567890123LL);

    EXPECT_EQ(span.annotation(2).key(), 103);
    EXPECT_EQ(span.annotation(2).value().stringvalue(), "string-value");

    const auto& string_string = span.annotation(3).value().stringstringvalue();
    EXPECT_EQ(span.annotation(3).key(), 104);
    EXPECT_EQ(string_string.stringvalue1().value(), "left");
    EXPECT_EQ(string_string.stringvalue2().value(), "right");

    const auto& int_string_string = span.annotation(4).value().intstringstringvalue();
    EXPECT_EQ(span.annotation(4).key(), 105);
    EXPECT_EQ(int_string_string.intvalue(), 7);
    EXPECT_EQ(int_string_string.stringvalue1().value(), "method");
    EXPECT_EQ(int_string_string.stringvalue2().value(), "GET");

    const auto& complex_value = span.annotation(5).value().longintintbytebytestringvalue();
    EXPECT_EQ(span.annotation(5).key(), 106);
    EXPECT_EQ(complex_value.longvalue(), 99);
    EXPECT_EQ(complex_value.intvalue1(), 1);
    EXPECT_EQ(complex_value.intvalue2(), 2);
    EXPECT_EQ(complex_value.bytevalue1(), 3);
    EXPECT_EQ(complex_value.bytevalue2(), 4);
    EXPECT_EQ(complex_value.stringvalue().value(), "rpc");

    const auto& bytes_value = span.annotation(6).value().bytesstringstringvalue();
    EXPECT_EQ(span.annotation(6).key(), 107);
    ASSERT_EQ(bytes_value.bytesvalue().size(), uid.size());
    EXPECT_EQ(static_cast<unsigned char>(bytes_value.bytesvalue()[0]), uid[0]);
    EXPECT_EQ(static_cast<unsigned char>(bytes_value.bytesvalue()[1]), uid[1]);
    EXPECT_EQ(static_cast<unsigned char>(bytes_value.bytesvalue()[2]), uid[2]);
    EXPECT_EQ(static_cast<unsigned char>(bytes_value.bytesvalue()[3]), uid[3]);
    EXPECT_EQ(bytes_value.stringvalue1().value(), "sql");
    EXPECT_EQ(bytes_value.stringvalue2().value(), "args");
}

TEST_F(GrpcMockTest, GrpcSpanBatchSerializesSpanEventAnnotationsTest) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_batch.size = 1;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 100;
    cfg->collector.span_batch.max_concurrent_requests = 2;

    TestableGrpcSpan span_client(mock_agent_service_.get());
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    span_client.setMockSpanStub(std::move(fake_stub));

    auto span_parent = std::make_shared<SpanImpl>(mock_agent_service_.get(), "event-annotation-op", "test-rpc");
    auto span_data = span_parent->getSpanData();
    // Directly-constructed SpanImpl bypasses NewSpan/extractContext, so seed a
    // trace id the way production would before this span is serialized.
    span_data->setTraceId(mock_agent_service_->generateTraceId());
    auto span_event = make_test_span_event_unique(*span_parent, "child-op");
    span_event->SetAnnotation(201, "event-annotation");
    span_data->addSpanEvent(std::move(span_event));
    span_data->finishSpanEvent();
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data, true));

    std::thread worker([&span_client] { span_client.sendSpanWorker(); });

    ASSERT_TRUE(fake->waitForBatchCount(1, std::chrono::seconds(2)));

    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();

    const auto request = fake->request(0);
    ASSERT_EQ(request.span_size(), 1);
    ASSERT_TRUE(request.span(0).has_span());
    const auto& span = request.span(0).span();
    ASSERT_EQ(span.spanevent_size(), 1);
    const auto& event = span.spanevent(0);
    ASSERT_EQ(event.annotation_size(), 1);
    EXPECT_EQ(event.annotation(0).key(), 201);
    EXPECT_EQ(event.annotation(0).value().stringvalue(), "event-annotation");
}

TEST_F(GrpcMockTest, GrpcSpanBatchSizeSplitTest) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_batch.size = 2;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 100;
    cfg->collector.span_batch.max_concurrent_requests = 4;

    TestableGrpcSpan span_client(mock_agent_service_.get());
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    span_client.setMockSpanStub(std::move(fake_stub));

    for (int i = 0; i < 4; i++) {
        auto span_data = make_test_span_data_ptr(*mock_agent_service_, "split-op-" + std::to_string(i));
        span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data, true));
    }

    std::thread worker([&span_client] { span_client.sendSpanWorker(); });

    ASSERT_TRUE(fake->waitForBatchCount(2, std::chrono::seconds(2)))
        << "4 queued chunks with batch size 2 should produce 2 batches";

    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();

    EXPECT_EQ(fake->request(0).span_size(), 2);
    EXPECT_EQ(fake->request(1).span_size(), 2);
}

TEST_F(GrpcMockTest, GrpcSpanQueueOverflowHeadDropTest) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->span.queue_size = 2;
    cfg->collector.span_batch.size = 10;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 50;
    cfg->collector.span_batch.max_concurrent_requests = 2;

    TestableGrpcSpan span_client(mock_agent_service_.get());
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    span_client.setMockSpanStub(std::move(fake_stub));

    // MockAgentService::cacheApi assigns sequential api ids per unique
    // operation name starting at 100, which identifies each span below.
    std::vector<int32_t> api_ids;
    for (int i = 0; i < 3; i++) {
        auto span_data = make_test_span_data_ptr(*mock_agent_service_, "overflow-op-" + std::to_string(i));
        api_ids.push_back(span_data->getApiId());
        span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data, true));
    }

    std::thread worker([&span_client] { span_client.sendSpanWorker(); });

    ASSERT_TRUE(fake->waitForBatchCount(1, std::chrono::seconds(2)));

    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();

    // Head-drop: the oldest chunk (overflow-op-0) was discarded on overflow
    const auto request = fake->request(0);
    ASSERT_EQ(request.span_size(), 2);
    EXPECT_EQ(request.span(0).span().apiid(), api_ids[1]);
    EXPECT_EQ(request.span(1).span().apiid(), api_ids[2]);
}

TEST_F(GrpcMockTest, GrpcSpanPermitExhaustionDropsBatchTest) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_batch.size = 1;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 10;
    cfg->collector.span_batch.max_concurrent_requests = 1;

    TestableGrpcSpan span_client(mock_agent_service_.get());
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    fake->setReplyMode(FakeSpanStub::ReplyMode::HOLD);
    span_client.setMockSpanStub(std::move(fake_stub));

    std::thread worker([&span_client] { span_client.sendSpanWorker(); });

    // First batch acquires the only permit; its callback is held by the fake
    auto span_data1 = make_test_span_data_ptr(*mock_agent_service_, "permit-op-1");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data1, true));
    ASSERT_TRUE(fake->waitForBatchCount(1, std::chrono::seconds(2)));

    // Second batch cannot acquire a permit within flush_interval_ms and is dropped
    auto span_data2 = make_test_span_data_ptr(*mock_agent_service_, "permit-op-2");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data2, true));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(fake->batchCount(), 1u) << "Batch should be dropped while the permit is in flight";

    // Completing the in-flight RPC returns the permit; the next batch goes out
    fake->releaseHeldCallbacks(grpc::Status::OK);
    auto span_data3 = make_test_span_data_ptr(*mock_agent_service_, "permit-op-3");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data3, true));
    ASSERT_TRUE(fake->waitForBatchCount(2, std::chrono::seconds(2)))
        << "Releasing the permit should allow the next batch to be sent";

    // Release the second held callback so shutdown does not wait on permits
    fake->releaseHeldCallbacks(grpc::Status::OK);

    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();

    EXPECT_EQ(fake->batchCount(), 2u);
}

TEST_F(GrpcMockTest, GrpcSpanOutOfOrderCompletionReleasesPermitTest) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_batch.size = 1;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 10;
    cfg->collector.span_batch.max_concurrent_requests = 3;

    TestableGrpcSpan span_client(mock_agent_service_.get());
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    fake->setReplyMode(FakeSpanStub::ReplyMode::HOLD);
    span_client.setMockSpanStub(std::move(fake_stub));

    std::thread worker([&span_client] { span_client.sendSpanWorker(); });

    for (int i = 0; i < 3; ++i) {
        auto span_data = make_test_span_data_ptr(
            *mock_agent_service_, "out-of-order-op-" + std::to_string(i));
        span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data, true));
    }
    ASSERT_TRUE(fake->waitForBatchCount(3, std::chrono::seconds(2)));

    ASSERT_TRUE(fake->releaseHeldCallback(1, grpc::Status::OK));
    auto next_span_data = make_test_span_data_ptr(*mock_agent_service_, "out-of-order-op-next");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(next_span_data, true));
    ASSERT_TRUE(fake->waitForBatchCount(4, std::chrono::seconds(2)))
        << "Completing a non-front in-flight call should release one permit";

    fake->releaseHeldCallbacks(grpc::Status::OK);
    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();
}

TEST_F(GrpcMockTest, GrpcSpanSynchronousLaunchFailureReleasesPermitTest) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_batch.size = 1;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 10;
    cfg->collector.span_batch.max_concurrent_requests = 1;

    TestableGrpcSpan span_client(mock_agent_service_.get());
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    fake->setReplyMode(FakeSpanStub::ReplyMode::THROW_BEFORE_CALLBACK);
    span_client.setMockSpanStub(std::move(fake_stub));

    std::thread worker([&span_client] { span_client.sendSpanWorker(); });

    auto failed_span_data = make_test_span_data_ptr(*mock_agent_service_, "launch-failure-op");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(failed_span_data, true));
    ASSERT_TRUE(fake->waitForBatchCount(1, std::chrono::seconds(2)));

    fake->setReplyMode(FakeSpanStub::ReplyMode::OK_EMPTY);
    auto next_span_data = make_test_span_data_ptr(*mock_agent_service_, "launch-recovery-op");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(next_span_data, true));
    ASSERT_TRUE(fake->waitForBatchCount(2, std::chrono::seconds(2)))
        << "A synchronous launch failure should remove its registry entry and return the permit";

    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();
}

TEST_F(GrpcMockTest, GrpcSpanErrorStatusReleasesPermitTest) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_batch.size = 1;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 10;
    cfg->collector.span_batch.max_concurrent_requests = 1;

    TestableGrpcSpan span_client(mock_agent_service_.get());
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    fake->setReplyMode(FakeSpanStub::ReplyMode::ERROR_STATUS);
    span_client.setMockSpanStub(std::move(fake_stub));

    std::thread worker([&span_client] { span_client.sendSpanWorker(); });

    auto span_data1 = make_test_span_data_ptr(*mock_agent_service_, "error-op-1");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data1, true));
    ASSERT_TRUE(fake->waitForBatchCount(1, std::chrono::seconds(2)));

    // A failed RPC must release its permit, or this second batch could never be sent
    auto span_data2 = make_test_span_data_ptr(*mock_agent_service_, "error-op-2");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data2, true));
    ASSERT_TRUE(fake->waitForBatchCount(2, std::chrono::seconds(2)))
        << "Permit should be released after an RPC failure";

    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();
}

TEST_F(GrpcMockTest, GrpcSpanPartialSuccessHandledTest) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_batch.size = 1;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 10;
    cfg->collector.span_batch.max_concurrent_requests = 1;

    TestableGrpcSpan span_client(mock_agent_service_.get());
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    fake->setReplyMode(FakeSpanStub::ReplyMode::OK_PARTIAL_SUCCESS);
    span_client.setMockSpanStub(std::move(fake_stub));

    std::thread worker([&span_client] { span_client.sendSpanWorker(); });

    auto span_data1 = make_test_span_data_ptr(*mock_agent_service_, "partial-success-op-1");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data1, true));
    ASSERT_TRUE(fake->waitForBatchCount(1, std::chrono::seconds(2)));

    // partial_success is observability-only: the worker keeps going and the
    // permit is released, so a following batch still goes out
    auto span_data2 = make_test_span_data_ptr(*mock_agent_service_, "partial-success-op-2");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data2, true));
    ASSERT_TRUE(fake->waitForBatchCount(2, std::chrono::seconds(2)));

    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();
}

// ============================================================
// GrpcAgent AgentInfo scheduler tests
// ============================================================

TEST_F(GrpcMockTest, GrpcAgentInfoSchedulerResendsPeriodically) {
    auto cfg = mock_agent_service_->mutableConfig();
    cfg->collector.agent_info.refresh_interval_ms = 20;
    cfg->collector.agent_info.send_retry_interval_ms = 10;
    cfg->collector.agent_info.max_try_per_attempt = 1;

    CountingAgentInfoGrpcAgent grpc_agent(cfg, SEND_OK);
    grpc_agent.setAgentService(mock_agent_service_.get());

    grpc_agent.startAgentInfo();
    EXPECT_TRUE(wait_for_condition([&] { return grpc_agent.calls() >= 2; }, std::chrono::seconds(2)))
        << "the scheduler should re-send AgentInfo every refresh interval";

    grpc_agent.stopAgentInfo();
}

TEST_F(GrpcMockTest, GrpcAgentInfoSchedulerToleratesSendFailure) {
    auto cfg = mock_agent_service_->mutableConfig();
    cfg->collector.agent_info.refresh_interval_ms = 20;
    cfg->collector.agent_info.send_retry_interval_ms = 10;
    cfg->collector.agent_info.max_try_per_attempt = 1;

    CountingAgentInfoGrpcAgent grpc_agent(cfg, SEND_FAIL);
    grpc_agent.setAgentService(mock_agent_service_.get());

    grpc_agent.startAgentInfo();
    EXPECT_TRUE(wait_for_condition([&] { return grpc_agent.calls() >= 2; }, std::chrono::seconds(2)))
        << "a failed post-boot re-send must not stop the scheduler";

    grpc_agent.stopAgentInfo();
}

TEST_F(GrpcMockTest, GrpcAgentInfoSchedulerSurvivesRegisterException) {
    auto cfg = mock_agent_service_->mutableConfig();
    cfg->collector.agent_info.refresh_interval_ms = 20;
    cfg->collector.agent_info.send_retry_interval_ms = 10;
    cfg->collector.agent_info.max_try_per_attempt = 1;

    ThrowingAgentInfoGrpcAgent grpc_agent(cfg);
    std::promise<void> success_promise;
    auto success = success_promise.get_future();
    grpc_agent.setSuccessPromise(&success_promise);
    grpc_agent.setAgentService(mock_agent_service_.get());

    grpc_agent.startAgentInfo();

    // The first re-send throws out of the scheduler loop. The supervisor must
    // restart it after WORKER_RESTART_DELAY instead of letting it die, so a
    // later cycle still delivers the AgentInfo.
    EXPECT_EQ(success.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "AgentInfo scheduler should survive a thrown registerAgent()";
    grpc_agent.stopAgentInfo();
    EXPECT_GE(grpc_agent.calls(), 2);
}

TEST_F(GrpcMockTest, GrpcAgentStartAgentInfoIsIdempotentAndDoesNotSendAtStart) {
    auto cfg = mock_agent_service_->mutableConfig();
    cfg->collector.agent_info.refresh_interval_ms = 60 * 1000;
    cfg->collector.agent_info.send_retry_interval_ms = 10;
    cfg->collector.agent_info.max_try_per_attempt = 1;

    CountingAgentInfoGrpcAgent grpc_agent(cfg, SEND_OK);
    grpc_agent.setAgentService(mock_agent_service_.get());

    grpc_agent.startAgentInfo();
    // A second start must be a no-op: spawning over the live scheduler thread
    // would std::terminate on the std::thread assignment.
    grpc_agent.startAgentInfo();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(grpc_agent.calls(), 0)
        << "boot registration happens outside the scheduler; the scheduler "
           "must wait out the refresh interval before its first re-send";

    grpc_agent.stopAgentInfo();
}

TEST_F(GrpcMockTest, GrpcAgentStopAgentInfoDuringRetriesReturnsPromptly) {
    auto cfg = mock_agent_service_->mutableConfig();
    cfg->collector.agent_info.refresh_interval_ms = 20;
    cfg->collector.agent_info.send_retry_interval_ms = 60 * 1000;
    cfg->collector.agent_info.max_try_per_attempt = 3;

    CountingAgentInfoGrpcAgent grpc_agent(cfg, SEND_FAIL);
    grpc_agent.setAgentService(mock_agent_service_.get());

    grpc_agent.startAgentInfo();
    ASSERT_TRUE(wait_for_condition([&] { return grpc_agent.calls() >= 1; }, std::chrono::seconds(2)));

    // The worker is now sleeping on the 60s retry delay; stop must wake it
    const auto stop_start = std::chrono::steady_clock::now();
    grpc_agent.stopAgentInfo();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_start;

    EXPECT_LT(stop_elapsed, std::chrono::seconds(2))
        << "stopAgentInfo should interrupt the retry delay instead of waiting it out";
}

TEST_F(GrpcMockTest, GrpcAgentRegisterWithRetryPacesAndStopsPromptly) {
    constexpr int retry_interval_ms = 100;
    auto cfg = mock_agent_service_->mutableConfig();
    cfg->collector.agent_info.send_retry_interval_ms = retry_interval_ms;

    CountingAgentInfoGrpcAgent grpc_agent(cfg, SEND_FAIL);
    grpc_agent.setAgentService(mock_agent_service_.get());

    std::atomic<bool> boot_result{true};
    std::thread boot([&] { boot_result = grpc_agent.registerAgentWithRetry(); });

    ASSERT_TRUE(wait_for_condition([&] { return grpc_agent.calls() >= 1; }, std::chrono::seconds(2)))
        << "boot registration should attempt a send immediately";

    const auto observe_start = std::chrono::steady_clock::now();
    const int calls_before = grpc_agent.calls();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int calls_during = grpc_agent.calls() - calls_before;
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - observe_start).count();

    // Paced retries: at most elapsed/interval attempts, plus slack for
    // read-order jitter. A hot spin racks up thousands of attempts in the
    // same window.
    const int max_paced = static_cast<int>(elapsed_ms / retry_interval_ms) + 2;
    EXPECT_LE(calls_during, max_paced)
        << "boot registration retries must stay paced during a collector outage";

    // stopAgentInfo must wake the boot retry wait even though no scheduler
    // thread was ever started.
    const auto stop_start = std::chrono::steady_clock::now();
    grpc_agent.stopAgentInfo();
    boot.join();
    EXPECT_LT(std::chrono::steady_clock::now() - stop_start, std::chrono::seconds(2))
        << "stopAgentInfo should interrupt the boot retry wait";
    EXPECT_FALSE(boot_result) << "an interrupted boot registration reports failure";
}

// ============================================================
// Collector outage tests: worker behavior while the channel cannot
// become ready, and recovery once it can again.
// ============================================================

TEST_F(GrpcMockTest, GrpcSpanCollectorOutageDropsBatchAndRecoveryResumesSending) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_batch.size = 1;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 10;
    cfg->collector.span_batch.max_concurrent_requests = 2;

    TestableGrpcSpan span_client(mock_agent_service_.get());
    span_client.setReadyChannel(false);
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    span_client.setMockSpanStub(std::move(fake_stub));

    std::thread worker([&span_client] { span_client.sendSpanWorker(); });

    // Outage policy: a batch collected while the channel cannot become ready
    // is dropped (not retried), and the worker keeps running.
    auto outage_span = make_test_span_data_ptr(*mock_agent_service_, "outage-op");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(outage_span, true));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(fake->batchCount(), 0u)
        << "a batch collected during an outage must not be sent";

    // Collector recovery: newly enqueued spans flow again.
    span_client.setReadyChannel(true);
    auto recovery_span_data = make_test_span_data_ptr(*mock_agent_service_, "recovery-op");
    const auto recovery_api_id = recovery_span_data->getApiId();
    span_client.enqueueSpan(std::make_unique<SpanChunk>(recovery_span_data, true));
    ASSERT_TRUE(fake->waitForBatchCount(1, std::chrono::seconds(2)))
        << "spans enqueued after channel recovery should be sent";

    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();

    ASSERT_EQ(fake->batchCount(), 1u);
    const auto request = fake->request(0);
    ASSERT_EQ(request.span_size(), 1);
    EXPECT_EQ(request.span(0).span().apiid(), recovery_api_id)
        << "the batch dropped during the outage must not reappear after recovery";
}

TEST_F(GrpcMockTest, GrpcSpanShutdownDuringCollectorOutageDropsRemainingSpans) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_batch.size = 2;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 10;
    cfg->collector.span_batch.max_concurrent_requests = 2;

    TestableGrpcSpan span_client(mock_agent_service_.get());
    span_client.setReadyChannel(false);
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    span_client.setMockSpanStub(std::move(fake_stub));

    for (int i = 0; i < 3; ++i) {
        auto span_data = make_test_span_data_ptr(
            *mock_agent_service_, "shutdown-outage-op-" + std::to_string(i));
        span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data, true));
    }

    // Stop before the worker runs: the queued spans are still pending when
    // the shutdown flush executes, and no channel was ever opened (mirrors a
    // collector that was down for the whole run), so the flush must drop
    // them instead of sending into the outage.
    span_client.stopSpanWorker();

    const auto start = std::chrono::steady_clock::now();
    std::thread worker([&span_client] { span_client.sendSpanWorker(); });
    worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(fake->batchCount(), 0u)
        << "remaining spans must be dropped, not sent, while the channel is down";
    EXPECT_LT(elapsed, std::chrono::seconds(3))
        << "the shutdown flush must not block waiting for a dead collector";
}

TEST_F(GrpcMockTest, GrpcMetadataResendsAfterChannelRecoveryWithoutCacheEviction) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(std::chrono::milliseconds(50));
    // Two readiness checks fail (collector down), then the channel recovers.
    // Recovery lands within the retry budget (3 retries), so the meta must
    // survive the outage on the retry schedule and still be delivered.
    metadata.setReadyChannelFailures(2);

    std::atomic<int> sends{0};
    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
    EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
        .WillOnce(DoAll(InvokeWithoutArgs([&sends] { ++sends; }),
                        SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));

    metadata.setMockMetaStub(std::move(mock_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api.outage.recovery"));

    std::thread meta_worker([&metadata]() { metadata.sendMetaWorker(); });

    EXPECT_TRUE(wait_for_condition([&sends] { return sends.load() >= 1; }, std::chrono::seconds(5)))
        << "metadata should be re-sent once the channel recovers";

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(sends.load(), 1);
    EXPECT_EQ(mock_agent_service_->removed_api_count_, 0)
        << "metadata delivered after channel recovery must keep its cache entry";
}

TEST_F(GrpcMockTest, GrpcCommandWorkerReconnectsAfterCollectorStreamFailure) {
    TestableGrpcCommand command(mock_agent_service_.get());

    auto mock_command_stub = std::make_unique<NiceMock<v1::MockProfilerCommandServiceStub>>();

    // First stream: the collector drops the connection right away, the way a
    // collector restart mid-stream surfaces to the worker.
    auto* dying_stream = new NiceMock<MockCmdStream>();
    EXPECT_CALL(*dying_stream, Read(_)).WillRepeatedly(Return(false));
    EXPECT_CALL(*dying_stream, Finish())
        .WillOnce(Return(grpc::Status(grpc::StatusCode::UNAVAILABLE, "collector down")));

    std::promise<void> reconnected;
    std::atomic<bool> reconnected_once{false};
    EXPECT_CALL(*mock_command_stub, HandleCommandV2Raw(_))
        .WillOnce(Return(dying_stream))
        .WillRepeatedly(Invoke([&](grpc::ClientContext* ctx) {
            if (!reconnected_once.exchange(true)) {
                reconnected.set_value();
            }
            return make_idle_cmd_stream(ctx);
        }));

    command.setMockCommandStub(std::move(mock_command_stub));

    std::thread worker([&command] { command.commandWorker(); });

    // The reconnect delay is the default exponential backoff (3s initial,
    // ±30% jitter), so allow generous headroom for the second connect.
    EXPECT_EQ(reconnected.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "the command worker should reconnect after the collector drops the stream";

    mock_agent_service_->setExiting(true);
    command.stopCommandWorker();
    if (worker.joinable()) worker.join();
}

TEST_F(GrpcMockTest, GrpcAgentPingWorkerKeepsRetryingWhileCollectorUnreachable) {
    CountingNotReadyGrpcAgent grpc_agent(mock_agent_service_->getConfig());
    grpc_agent.setAgentService(mock_agent_service_.get());

    std::thread ping_worker([&grpc_agent] { grpc_agent.sendPingWorker(); });

    // Each failed stream start consumes one readiness attempt; the supervisor
    // must retry after WORKER_RESTART_DELAY instead of ending the worker for
    // the process lifetime.
    EXPECT_TRUE(wait_for_condition(
        [&grpc_agent] { return grpc_agent.readyAttempts() >= 2; }, std::chrono::seconds(5)))
        << "the ping worker must keep retrying while the collector is unreachable";

    const auto stop_start = std::chrono::steady_clock::now();
    grpc_agent.stopPingWorker();
    ping_worker.join();
    EXPECT_LT(std::chrono::steady_clock::now() - stop_start, std::chrono::seconds(2))
        << "stopPingWorker must wake the outage retry delay promptly";
}

TEST_F(GrpcMockTest, GrpcStatsWorkerKeepsRetryingWhileCollectorUnreachable) {
    CountingNotReadyGrpcStats stats_client(mock_agent_service_->getConfig());
    stats_client.setAgentService(mock_agent_service_.get());

    std::thread stats_worker([&stats_client] { stats_client.sendStatsWorker(); });

    EXPECT_TRUE(wait_for_condition(
        [&stats_client] { return stats_client.readyAttempts() >= 2; }, std::chrono::seconds(5)))
        << "the stats worker must keep retrying while the collector is unreachable";

    const auto stop_start = std::chrono::steady_clock::now();
    stats_client.stopStatsWorker();
    stats_worker.join();
    EXPECT_LT(std::chrono::steady_clock::now() - stop_start, std::chrono::seconds(2))
        << "stopStatsWorker must wake the outage retry delay promptly";
}

// ============================================================
// GrpcMetadata queue boundary tests
// ============================================================

TEST_F(GrpcMockTest, GrpcMetadataQueueOverflowDropsNewMeta) {
    mock_agent_service_->mutableConfig()->collector.grpc.channel.sender_queue_size = 2;

    TestableGrpcMetadata metadata(mock_agent_service_.get());

    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
    // Only the 2 queued metas may be sent; the third was dropped on enqueue
    EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
        .Times(2)
        .WillRepeatedly(DoAll(SetArgPointee<2>(success_result()), Return(grpc::Status::OK)));

    metadata.setMockMetaStub(std::move(mock_meta_stub));

    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "overflow.api.1"));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 2, 100, "overflow.api.2"));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 3, 100, "overflow.api.3"));

    std::thread meta_worker([&metadata]() { metadata.sendMetaWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();
}

TEST_F(GrpcMockTest, GrpcMetadataEnqueueNullMetaIsNoop) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());

    auto mock_meta_stub = std::make_unique<StrictMock<v1::MockMetadataStub>>();
    metadata.setMockMetaStub(std::move(mock_meta_stub));

    metadata.enqueueMeta(nullptr);

    SUCCEED() << "Null metadata must be ignored without touching the queue or stub";
}

// ============================================================
// GrpcClientTuning injection tests: each verifies that a knob that used to be
// a hardcoded constant is actually honored when injected.
// ============================================================

TEST_F(GrpcMockTest, GrpcAgentUnaryRequestUsesInjectedDeadline) {
    GrpcClientTuning tuning;
    tuning.request_timeout = std::chrono::milliseconds(250);
    TestableGrpcAgent agent(mock_agent_service_.get(), tuning);

    // The remaining budget is measured inside the stub call, right after
    // set_request_deadline() ran, so slow AgentInfo construction (host name /
    // ip resolution) cannot inflate it.
    std::chrono::milliseconds captured_budget{};
    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(Invoke([&captured_budget](grpc::ClientContext* ctx,
                                            const v1::PAgentInfo&,
                                            v1::PResult*) {
            captured_budget = std::chrono::duration_cast<std::chrono::milliseconds>(
                ctx->deadline() - std::chrono::system_clock::now());
            return grpc::Status::OK;
        }));
    agent.setMockAgentStub(std::move(mock_agent_stub));

    EXPECT_EQ(agent.registerAgent(), SEND_OK);

    // The budget must reflect the injected 250ms, not the 5s default.
    EXPECT_GT(captured_budget, std::chrono::milliseconds(0));
    EXPECT_LE(captured_budget, std::chrono::milliseconds(250));
}

TEST_F(GrpcMockTest, GrpcMetadataHonorsInjectedRetryLimit) {
    GrpcClientTuning tuning;
    tuning.meta_retry_delay = std::chrono::milliseconds(20);
    tuning.meta_retry_max_attempts = 1;
    TestableGrpcMetadata metadata(mock_agent_service_.get(), tuning);

    std::atomic<int> attempts{0};
    auto mock_meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
    // Initial send + exactly one scheduled retry (instead of the default 3)
    EXPECT_CALL(*mock_meta_stub, RequestApiMetaData(_, _, _))
        .Times(2)
        .WillRepeatedly(DoAll(InvokeWithoutArgs([&attempts] { ++attempts; }),
                              Return(grpc::Status(grpc::StatusCode::UNAVAILABLE, "unavailable"))));

    metadata.setMockMetaStub(std::move(mock_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api.injected.retry"));

    std::thread meta_worker([&metadata]() { metadata.sendMetaWorker(); });

    EXPECT_TRUE(wait_for_condition(
        [this] { return mock_agent_service_->removed_api_count_ >= 1; }, std::chrono::seconds(5)))
        << "the cache entry must be released after the injected retry budget is exhausted";

    // Give an unexpected extra retry the chance to fire before stopping, so
    // the Times(2) expectation would catch it.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(attempts.load(), 2);
    EXPECT_EQ(mock_agent_service_->removed_api_count_, 1);
}

TEST_F(GrpcMockTest, GrpcStatsHonorsInjectedQueueCapacity) {
    GrpcClientTuning tuning;
    tuning.max_stats_queue_size = 4;
    TestableGrpcStats stats_client(mock_agent_service_.get(), tuning);

    // Four payloads fit the injected capacity (the default is 2): no
    // overflow purge may be requested yet.
    for (int i = 0; i < 4; ++i) {
        stats_client.enqueueStats(AGENT_STATS);
    }
    EXPECT_FALSE(stats_client.emptyStatsQueueIfRequestedForTest())
        << "enqueues within the injected capacity must not request a purge";

    // The fifth payload overflows and marks the queued stats stale.
    stats_client.enqueueStats(AGENT_STATS);
    EXPECT_TRUE(stats_client.emptyStatsQueueIfRequestedForTest());
    EXPECT_FALSE(stats_client.emptyStatsQueueIfRequestedForTest());
}

TEST_F(GrpcMockTest, GrpcCommandHonorsInjectedActiveThreadCountStreamCap) {
    GrpcClientTuning tuning;
    tuning.max_active_thread_count_streams = 2;
    tuning.active_thread_count_flush_interval = std::chrono::milliseconds(20);
    TestableGrpcCommand command(mock_agent_service_.get(), tuning);

    auto mock_command_stub = std::make_unique<NiceMock<v1::MockProfilerCommandServiceStub>>();

    auto make_atc_request = [](int32_t request_id) {
        v1::PCmdRequest request;
        request.set_requestid(request_id);
        request.mutable_commandactivethreadcount();
        return request;
    };

    auto* stream = new NiceMock<MockCmdStream>();
    EXPECT_CALL(*stream, Read(_))
        .WillOnce(DoAll(SetArgPointee<0>(make_atc_request(101)), Return(true)))
        .WillOnce(DoAll(SetArgPointee<0>(make_atc_request(102)), Return(true)))
        .WillOnce(DoAll(SetArgPointee<0>(make_atc_request(103)), Return(true)))
        .WillRepeatedly(Return(false));

    // Only the request over the injected cap writes back a fail message.
    std::promise<void> fail_seen;
    v1::PCmdMessage fail_message;
    EXPECT_CALL(*stream, Write(_, _))
        .WillOnce(DoAll(SaveArg<0>(&fail_message),
                        InvokeWithoutArgs([&fail_seen] { fail_seen.set_value(); }),
                        Return(true)));

    EXPECT_CALL(*mock_command_stub, HandleCommandV2Raw(_))
        .WillOnce(Return(stream))
        .WillRepeatedly(Invoke(make_idle_cmd_stream));

    // The two admitted streams keep running (Write succeeds) so they still
    // occupy their slots when the third request arrives. The RPCs start
    // asynchronously on the stream threads, and a stream signaled to stop
    // before its RPC starts deliberately never starts it — so the test must
    // wait for both starts before stopping, or Times(2) races the shutdown.
    std::promise<void> both_streams_started;
    std::atomic<int> streams_started{0};
    EXPECT_CALL(*mock_command_stub, CommandStreamActiveThreadCountRaw(_, _))
        .Times(2)
        .WillRepeatedly(InvokeWithoutArgs([&both_streams_started, &streams_started] {
            auto* writer = new NiceMock<MockActiveThreadCountWriter>();
            ON_CALL(*writer, Write(_, _)).WillByDefault(Return(true));
            ON_CALL(*writer, WritesDone()).WillByDefault(Return(true));
            ON_CALL(*writer, Finish()).WillByDefault(Return(grpc::Status::OK));
            if (streams_started.fetch_add(1) + 1 == 2) {
                both_streams_started.set_value();
            }
            return writer;
        }));

    command.setMockCommandStub(std::move(mock_command_stub));

    std::thread worker([&command] { command.commandWorker(); });

    EXPECT_EQ(fail_seen.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "the third stream must be rejected once the injected cap of 2 is reached";
    EXPECT_EQ(both_streams_started.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "both admitted streams must start their RPCs before the shutdown races them";

    mock_agent_service_->setExiting(true);
    command.stopCommandWorker();
    if (worker.joinable()) worker.join();

    ASSERT_TRUE(fail_message.has_failmessage());
    EXPECT_EQ(fail_message.failmessage().responseid(), 103);
    EXPECT_EQ(fail_message.failmessage().message().value(), "too many active thread count streams");
}

TEST_F(GrpcMockTest, RequestStopCommandWorkerDoesNotJoinBlockedActiveThreadCountStream) {
    GrpcClientTuning tuning;
    tuning.active_thread_count_flush_interval = std::chrono::milliseconds(20);
    TestableGrpcCommand command(mock_agent_service_.get(), tuning);

    auto mock_command_stub = std::make_unique<NiceMock<v1::MockProfilerCommandServiceStub>>();

    v1::PCmdRequest atc_request;
    atc_request.set_requestid(201);
    atc_request.mutable_commandactivethreadcount();

    auto* stream = new NiceMock<MockCmdStream>();
    EXPECT_CALL(*stream, Read(_))
        .WillOnce(DoAll(SetArgPointee<0>(atc_request), Return(true)))
        .WillRepeatedly(Return(false));

    EXPECT_CALL(*mock_command_stub, HandleCommandV2Raw(_))
        .WillOnce(Return(stream))
        .WillRepeatedly(Invoke(make_idle_cmd_stream));

    // The writer simulates a collector that does not honor cancellation: the
    // first Write parks the stream thread until the test releases it, so the
    // thread is provably still running when requestStopCommandWorker() is
    // called. Before the request/stop split, the signal phase joined this
    // thread and shutdown hung for as long as the collector stalled.
    std::promise<void> write_entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    EXPECT_CALL(*mock_command_stub, CommandStreamActiveThreadCountRaw(_, _))
        .WillOnce(InvokeWithoutArgs([&write_entered, released] {
            auto* writer = new NiceMock<MockActiveThreadCountWriter>();
            EXPECT_CALL(*writer, Write(_, _))
                .WillOnce(InvokeWithoutArgs([&write_entered, released] {
                    write_entered.set_value();
                    released.wait();
                    return false;
                }))
                .WillRepeatedly(Return(false));
            ON_CALL(*writer, WritesDone()).WillByDefault(Return(true));
            ON_CALL(*writer, Finish()).WillByDefault(Return(grpc::Status::OK));
            return writer;
        }));

    command.setMockCommandStub(std::move(mock_command_stub));

    std::thread worker([&command] { command.commandWorker(); });

    ASSERT_EQ(write_entered.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "active thread count stream should have started and entered Write";

    // Run the signal phase from a helper thread: a regression that joins the
    // blocked stream surfaces as a failed wait_for below (the release after it
    // unblocks the join, so the test still finishes) instead of a hung test.
    auto request_stop_done = std::async(std::launch::async, [&command] {
        command.requestStopCommandWorker();
    });
    EXPECT_EQ(request_stop_done.wait_for(std::chrono::seconds(2)), std::future_status::ready)
        << "requestStopCommandWorker must not join the blocked active thread count stream";

    release.set_value();
    request_stop_done.wait();
    mock_agent_service_->setExiting(true);
    command.stopCommandWorker();
    if (worker.joinable()) worker.join();
}

TEST_F(GrpcMockTest, GrpcSpanShutdownHonorsInjectedAwaitTimeout) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_batch.size = 1;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 10;
    cfg->collector.span_batch.max_concurrent_requests = 1;

    GrpcClientTuning tuning;
    tuning.span_shutdown_await_timeout = std::chrono::milliseconds(50);
    TestableGrpcSpan span_client(mock_agent_service_.get(), tuning);
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    fake->setReplyMode(FakeSpanStub::ReplyMode::HOLD);
    span_client.setMockSpanStub(std::move(fake_stub));

    std::thread worker([&span_client] { span_client.sendSpanWorker(); });

    auto span_data = make_test_span_data_ptr(*mock_agent_service_, "shutdown-await-op");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data, true));
    ASSERT_TRUE(fake->waitForBatchCount(1, std::chrono::seconds(2)));

    // The held callback keeps the only permit checked out, so the shutdown
    // flush must give up after the two injected 50ms waits (cancel in
    // between) instead of the production 3s+3s.
    const auto stop_start = std::chrono::steady_clock::now();
    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();
    EXPECT_LT(std::chrono::steady_clock::now() - stop_start, std::chrono::seconds(2))
        << "shutdown must be paced by the injected await timeout";

    // A completion delivered after shutdown only touches the shared in-flight
    // state and stays safe.
    fake->releaseHeldCallbacks(grpc::Status::CANCELLED);
}

TEST_F(GrpcMockTest, GrpcAgentPingWorkerRestartHonorsInjectedDelay) {
    GrpcClientTuning tuning;
    tuning.worker_restart_delay = std::chrono::milliseconds(10);
    ThrowingReadyGrpcAgent agent(mock_agent_service_->getConfig(), tuning);
    agent.setAgentService(mock_agent_service_.get());

    std::thread ping_worker([&agent]() { EXPECT_NO_THROW(agent.sendPingWorker()); });

    // Every attempt throws in readyChannel(). With the production 1s restart
    // delay 8 supervised restarts would need ~7s; the injected 10ms delay
    // must reach them well within the wait budget.
    EXPECT_TRUE(wait_for_condition([&agent] { return agent.attempts() >= 8; },
                                   std::chrono::seconds(3)))
        << "supervised restarts must pace by the injected delay";

    agent.stopPingWorker();
    if (ping_worker.joinable()) ping_worker.join();
}

TEST_F(GrpcMockTest, GrpcCommandReconnectHonorsInjectedBackoff) {
    GrpcClientTuning tuning;
    tuning.reconnect_initial_interval = std::chrono::milliseconds(10);
    tuning.reconnect_multiplier = 1.0;
    tuning.reconnect_randomization_factor = 0.0;
    tuning.reconnect_max_interval = std::chrono::milliseconds(10);
    TestableGrpcCommand command(mock_agent_service_.get(), tuning);

    std::atomic<int> connect_attempts{0};
    auto mock_command_stub = std::make_unique<NiceMock<v1::MockProfilerCommandServiceStub>>();
    EXPECT_CALL(*mock_command_stub, HandleCommandV2Raw(_))
        .WillRepeatedly(Invoke([&connect_attempts](grpc::ClientContext*)
                -> grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>* {
            ++connect_attempts;
            return nullptr;
        }));
    command.setMockCommandStub(std::move(mock_command_stub));

    std::thread worker([&command] { command.commandWorker(); });

    // With the production 3s initial reconnect interval, 8 connect attempts
    // would need ~21s; the injected 10ms cadence reaches them almost
    // immediately.
    EXPECT_TRUE(wait_for_condition([&connect_attempts] { return connect_attempts.load() >= 8; },
                                   std::chrono::seconds(3)))
        << "stream reconnects must pace by the injected backoff";

    mock_agent_service_->setExiting(true);
    command.stopCommandWorker();
    if (worker.joinable()) worker.join();
}

} // namespace pinpoint
