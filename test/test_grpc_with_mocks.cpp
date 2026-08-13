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
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <memory>
#include <new>
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

namespace {

// Single-shot allocation-failure injection for the metadata completion
// no-throw regression test, mirroring test_noop.cpp: arming makes the next
// operator new on this thread throw bad_alloc. gRPC's own threads allocate
// through the same replaced operator new but never see the thread-local
// flag, so only the arming (test) thread is affected.
thread_local bool fail_next_allocation = false;

void arm_allocation_failure() noexcept {
    fail_next_allocation = true;
}

// Disarms and reports whether the armed failure was consumed — i.e. whether
// the guarded window allocated at all on this thread.
bool clear_allocation_failure() noexcept {
    const bool was_consumed = !fail_next_allocation;
    fail_next_allocation = false;
    return was_consumed;
}

}  // namespace

void* operator new(std::size_t size) {
    if (fail_next_allocation) {
        fail_next_allocation = false;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size != 0 ? size : 1)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    if (fail_next_allocation) {
        fail_next_allocation = false;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size != 0 ? size : 1)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](size);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }

namespace pinpoint {

static v1::PResult success_result() {
    v1::PResult result;
    result.set_success(true);
    return result;
}

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

// Hand-written fake for the Metadata stub: the generated MockMetadataStub
// cannot serve the callback-based async()->Request*MetaData path used by
// GrpcMetadata (its async() returns nullptr), so this fake implements
// async_interface, records requests per RPC type and lets tests script
// per-type outcomes and control when each RPC's completion callback runs.
class FakeMetadataStub : public v1::Metadata::StubInterface {
public:
    enum class MetaRpc { API, STRING, SQL, SQL_UID, EXCEPTION };
    enum class ReplyMode { OK, RESULT_FAIL, ERROR_STATUS, HOLD };

    FakeMetadataStub() : fake_async_(this) {}

    async_interface* async() override { return &fake_async_; }

    // Default outcome for every RPC without a scripted reply.
    void setReplyMode(ReplyMode mode) {
        std::unique_lock<std::mutex> lock(mutex_);
        mode_ = mode;
    }

    // Scripted per-type outcomes, consumed FIFO before the default mode —
    // the async equivalent of gmock WillOnce chains.
    void pushReply(MetaRpc rpc, grpc::Status status, bool result_success) {
        std::unique_lock<std::mutex> lock(mutex_);
        scripted_[index(rpc)].push_back(ScriptedReply{std::move(status), result_success, false});
    }

    void pushThrow(MetaRpc rpc) {
        std::unique_lock<std::mutex> lock(mutex_);
        scripted_[index(rpc)].push_back(ScriptedReply{grpc::Status::OK, true, true});
    }

    size_t requestCount(MetaRpc rpc) {
        std::unique_lock<std::mutex> lock(mutex_);
        return counts_[index(rpc)];
    }

    bool waitForRequestCount(MetaRpc rpc, size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return counts_[index(rpc)] >= count; });
    }

    bool waitForTotalRequestCount(size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            size_t total = 0;
            for (const auto c : counts_) total += c;
            return total >= count;
        });
    }

    v1::PApiMetaData apiRequest(size_t i) {
        std::unique_lock<std::mutex> lock(mutex_);
        return api_requests_.at(i);
    }
    v1::PStringMetaData stringRequest(size_t i) {
        std::unique_lock<std::mutex> lock(mutex_);
        return string_requests_.at(i);
    }
    v1::PSqlMetaData sqlRequest(size_t i) {
        std::unique_lock<std::mutex> lock(mutex_);
        return sql_requests_.at(i);
    }
    v1::PSqlUidMetaData sqlUidRequest(size_t i) {
        std::unique_lock<std::mutex> lock(mutex_);
        return sql_uid_requests_.at(i);
    }
    v1::PExceptionMetaData exceptionRequest(size_t i) {
        std::unique_lock<std::mutex> lock(mutex_);
        return exception_requests_.at(i);
    }

    bool waitForHeldCallbacks(size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return held_.size() >= count; });
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

    // Releases one held callback, moving `status` into the invocation. The
    // completion no-throw regression test arms a single-shot allocation
    // failure around this call: the const& variant above would copy the
    // status strings in test code and consume the failure before it could
    // reach the production completion path under test.
    bool releaseHeldCallbackByMove(size_t index, grpc::Status&& status) {
        std::function<void(grpc::Status)> held;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (index >= held_.size()) {
                return false;
            }
            held = std::move(held_[index]);
            held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(index));
        }
        held(std::move(status));
        return true;
    }

    // Sync surface is unused by the async pipeline.
    grpc::Status RequestSqlMetaData(grpc::ClientContext*, const v1::PSqlMetaData&, v1::PResult*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "sync unused");
    }
    grpc::Status RequestSqlUidMetaData(grpc::ClientContext*, const v1::PSqlUidMetaData&, v1::PResult*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "sync unused");
    }
    grpc::Status RequestApiMetaData(grpc::ClientContext*, const v1::PApiMetaData&, v1::PResult*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "sync unused");
    }
    grpc::Status RequestStringMetaData(grpc::ClientContext*, const v1::PStringMetaData&, v1::PResult*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "sync unused");
    }
    grpc::Status RequestExceptionMetaData(grpc::ClientContext*, const v1::PExceptionMetaData&, v1::PResult*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "sync unused");
    }

private:
    struct ScriptedReply {
        grpc::Status status;
        bool result_success;
        bool throws;
    };

    static size_t index(MetaRpc rpc) { return static_cast<size_t>(rpc); }

    class FakeAsync : public v1::Metadata::StubInterface::async_interface {
    public:
        explicit FakeAsync(FakeMetadataStub* owner) : owner_(owner) {}
        void RequestSqlMetaData(grpc::ClientContext*, const v1::PSqlMetaData* request,
                                v1::PResult* response, std::function<void(grpc::Status)> on_done) override {
            owner_->handle(MetaRpc::SQL, owner_->sql_requests_, request, response, std::move(on_done));
        }
        void RequestSqlMetaData(grpc::ClientContext*, const v1::PSqlMetaData*, v1::PResult*,
                                grpc::ClientUnaryReactor*) override {}
        void RequestSqlUidMetaData(grpc::ClientContext*, const v1::PSqlUidMetaData* request,
                                   v1::PResult* response, std::function<void(grpc::Status)> on_done) override {
            owner_->handle(MetaRpc::SQL_UID, owner_->sql_uid_requests_, request, response, std::move(on_done));
        }
        void RequestSqlUidMetaData(grpc::ClientContext*, const v1::PSqlUidMetaData*, v1::PResult*,
                                   grpc::ClientUnaryReactor*) override {}
        void RequestApiMetaData(grpc::ClientContext*, const v1::PApiMetaData* request,
                                v1::PResult* response, std::function<void(grpc::Status)> on_done) override {
            owner_->handle(MetaRpc::API, owner_->api_requests_, request, response, std::move(on_done));
        }
        void RequestApiMetaData(grpc::ClientContext*, const v1::PApiMetaData*, v1::PResult*,
                                grpc::ClientUnaryReactor*) override {}
        void RequestStringMetaData(grpc::ClientContext*, const v1::PStringMetaData* request,
                                   v1::PResult* response, std::function<void(grpc::Status)> on_done) override {
            owner_->handle(MetaRpc::STRING, owner_->string_requests_, request, response, std::move(on_done));
        }
        void RequestStringMetaData(grpc::ClientContext*, const v1::PStringMetaData*, v1::PResult*,
                                   grpc::ClientUnaryReactor*) override {}
        void RequestExceptionMetaData(grpc::ClientContext*, const v1::PExceptionMetaData* request,
                                      v1::PResult* response, std::function<void(grpc::Status)> on_done) override {
            owner_->handle(MetaRpc::EXCEPTION, owner_->exception_requests_, request, response, std::move(on_done));
        }
        void RequestExceptionMetaData(grpc::ClientContext*, const v1::PExceptionMetaData*, v1::PResult*,
                                      grpc::ClientUnaryReactor*) override {}

    private:
        FakeMetadataStub* owner_;
    };

    template <typename Request>
    void handle(MetaRpc rpc, std::vector<Request>& store, const Request* request,
                v1::PResult* response, std::function<void(grpc::Status)> on_done) {
        std::function<void(grpc::Status)> to_invoke;
        grpc::Status status = grpc::Status::OK;
        bool throw_before_callback = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            store.push_back(*request);
            ++counts_[index(rpc)];

            auto& scripted = scripted_[index(rpc)];
            if (!scripted.empty()) {
                auto reply = std::move(scripted.front());
                scripted.pop_front();
                if (reply.throws) {
                    throw_before_callback = true;
                } else {
                    status = std::move(reply.status);
                    response->set_success(reply.result_success);
                    to_invoke = std::move(on_done);
                }
            } else {
                switch (mode_) {
                    case ReplyMode::OK:
                        response->set_success(true);
                        to_invoke = std::move(on_done);
                        break;
                    case ReplyMode::RESULT_FAIL:
                        response->set_success(false);
                        to_invoke = std::move(on_done);
                        break;
                    case ReplyMode::ERROR_STATUS:
                        status = grpc::Status(grpc::StatusCode::UNAVAILABLE, "fake unavailable");
                        to_invoke = std::move(on_done);
                        break;
                    case ReplyMode::HOLD:
                        // The response is pre-filled as a success so a later
                        // release with Status::OK completes the item; releasing
                        // with an error status fails it instead.
                        response->set_success(true);
                        held_.push_back(std::move(on_done));
                        break;
                }
            }
        }
        cv_.notify_all();
        if (throw_before_callback) {
            throw std::runtime_error("fake async metadata launch failure");
        }
        if (to_invoke) {
            to_invoke(status);
        }
    }

    grpc::ClientAsyncResponseReaderInterface<v1::PResult>* AsyncRequestSqlMetaDataRaw(
        grpc::ClientContext*, const v1::PSqlMetaData&, grpc::CompletionQueue*) override { return nullptr; }
    grpc::ClientAsyncResponseReaderInterface<v1::PResult>* PrepareAsyncRequestSqlMetaDataRaw(
        grpc::ClientContext*, const v1::PSqlMetaData&, grpc::CompletionQueue*) override { return nullptr; }
    grpc::ClientAsyncResponseReaderInterface<v1::PResult>* AsyncRequestSqlUidMetaDataRaw(
        grpc::ClientContext*, const v1::PSqlUidMetaData&, grpc::CompletionQueue*) override { return nullptr; }
    grpc::ClientAsyncResponseReaderInterface<v1::PResult>* PrepareAsyncRequestSqlUidMetaDataRaw(
        grpc::ClientContext*, const v1::PSqlUidMetaData&, grpc::CompletionQueue*) override { return nullptr; }
    grpc::ClientAsyncResponseReaderInterface<v1::PResult>* AsyncRequestApiMetaDataRaw(
        grpc::ClientContext*, const v1::PApiMetaData&, grpc::CompletionQueue*) override { return nullptr; }
    grpc::ClientAsyncResponseReaderInterface<v1::PResult>* PrepareAsyncRequestApiMetaDataRaw(
        grpc::ClientContext*, const v1::PApiMetaData&, grpc::CompletionQueue*) override { return nullptr; }
    grpc::ClientAsyncResponseReaderInterface<v1::PResult>* AsyncRequestStringMetaDataRaw(
        grpc::ClientContext*, const v1::PStringMetaData&, grpc::CompletionQueue*) override { return nullptr; }
    grpc::ClientAsyncResponseReaderInterface<v1::PResult>* PrepareAsyncRequestStringMetaDataRaw(
        grpc::ClientContext*, const v1::PStringMetaData&, grpc::CompletionQueue*) override { return nullptr; }
    grpc::ClientAsyncResponseReaderInterface<v1::PResult>* AsyncRequestExceptionMetaDataRaw(
        grpc::ClientContext*, const v1::PExceptionMetaData&, grpc::CompletionQueue*) override { return nullptr; }
    grpc::ClientAsyncResponseReaderInterface<v1::PResult>* PrepareAsyncRequestExceptionMetaDataRaw(
        grpc::ClientContext*, const v1::PExceptionMetaData&, grpc::CompletionQueue*) override { return nullptr; }

    FakeAsync fake_async_;
    std::mutex mutex_;
    std::condition_variable cv_;
    ReplyMode mode_{ReplyMode::OK};
    std::array<size_t, 5> counts_{};
    std::array<std::deque<ScriptedReply>, 5> scripted_{};
    std::vector<v1::PApiMetaData> api_requests_;
    std::vector<v1::PStringMetaData> string_requests_;
    std::vector<v1::PSqlMetaData> sql_requests_;
    std::vector<v1::PSqlUidMetaData> sql_uid_requests_;
    std::vector<v1::PExceptionMetaData> exception_requests_;
    std::vector<std::function<void(grpc::Status)>> held_;
};

// Testable gRPC classes that inject mock stubs
class TestableGrpcMetadata : public GrpcMetadata {
public:
    explicit TestableGrpcMetadata(AgentService* agent, const GrpcClientTuning& tuning = {})
        : GrpcMetadata(agent->getConfig(), tuning) {
        setAgentService(agent);
    }

    void setMockMetaStub(std::unique_ptr<v1::Metadata::StubInterface> mock_stub) {
        set_meta_stub(std::move(mock_stub));
    }

    bool readyChannel() override {
        if (ready_channel_throws_.load() > 0) {
            ready_channel_throws_.fetch_sub(1);
            throw std::runtime_error("injected metadata channel setup failure");
        }
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

    // Models a transient channel setup exception: the next N readiness
    // checks throw, after which the normal behavior applies again.
    void setReadyChannelThrows(int throws) { ready_channel_throws_ = throws; }

    // Retry-specific tests shrink these so retries fire in milliseconds; the
    // defaults match production so other tests never see an in-test retry.
    // Must be called before the worker thread starts.
    void setRetryDelay(std::chrono::milliseconds delay) { tuning_.meta_retry_delay = delay; }

private:
    // Toggled by the test body while the worker thread polls readyChannel();
    // atomic so TSan-clean, like ready_channel_failures_.
    std::atomic<bool> ready_channel_{true};
    std::atomic<int> ready_channel_failures_{0};
    std::atomic<int> ready_channel_throws_{0};
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

    void setMockMetaStub(std::unique_ptr<v1::Metadata::StubInterface> mock_stub) {
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
    explicit ThrowingReadyGrpcStats(std::shared_ptr<const Config> config,
                                    const GrpcClientTuning& tuning = {})
        : GrpcStats(std::move(config), tuning) {}

    bool readyChannel() override {
        attempts_.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("injected stats channel setup failure");
    }

    // Distinguishes supervised restart (attempts keep growing) from a worker
    // that died permanently after the first exception.
    int attempts() const { return attempts_.load(std::memory_order_relaxed); }

private:
    std::atomic<int> attempts_{0};
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

// Owns a worker thread and guarantees it is stopped and joined on scope exit.
// A fatal ASSERT between starting the worker and the test's explicit join
// returns from the test body early, and destroying a joinable std::thread
// std::terminates — one timed-out wait would take the whole binary (and every
// remaining test) down with it. The stop callback runs only when the guard
// itself has to join; the explicit stop-then-join sequences in the test
// bodies are unaffected (the stop functions are idempotent).
class ScopedWorker {
public:
    ScopedWorker(std::function<void()> stop, std::function<void()> body)
        : stop_(std::move(stop)), thread_(std::move(body)) {}
    ~ScopedWorker() {
        if (thread_.joinable()) {
            if (stop_) stop_();
            thread_.join();
        }
    }
    ScopedWorker(const ScopedWorker&) = delete;
    ScopedWorker& operator=(const ScopedWorker&) = delete;

    bool joinable() const { return thread_.joinable(); }
    void join() { thread_.join(); }

private:
    std::function<void()> stop_;
    std::thread thread_;
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
        cfg->agent_id_ = "test-agent-id";
        cfg->agent_name_ = "test-agent-name";
        mock_agent_service_->setAppName("test-app");
        mock_agent_service_->setAppType(1300);
        mock_agent_service_->setAgentId("test-agent-id");
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

TEST_F(GrpcMockTest, GrpcAgentPingWorkerContainsChannelSetupException) {
    ThrowingReadyGrpcAgent agent(mock_agent_service_->getConfig());
    agent.setAgentService(mock_agent_service_.get());

    // The worker must contain the exception and keep retrying (supervised
    // restart) instead of dying — so it only returns once stopped.
    ScopedWorker ping_worker([&agent] { agent.stopPingWorker(); },
                             [&agent] { EXPECT_NO_THROW(agent.sendPingWorker()); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    agent.stopPingWorker();
    ping_worker.join();
    EXPECT_FALSE(mock_agent_service_->isExiting());
}

TEST_F(GrpcMockTest, GrpcAgentMetaWorkerTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    agent.setMockMetaStub(std::move(fake_meta_stub));

    // Enqueue some metadata
    auto api_meta = std::make_unique<MetaData>(META_API, 1, 100, "test.api");
    agent.enqueueMeta(std::move(api_meta));

    auto str_meta = std::make_unique<MetaData>(META_STRING, 2, "test.string", STRING_META_ERROR);
    agent.enqueueMeta(std::move(str_meta));

    // Test meta worker operations
    ScopedWorker meta_worker([&agent] { agent.stopMetaWorker(); },
                             [&agent] { agent.sendMetaWorker(); });

    EXPECT_TRUE(fake->waitForTotalRequestCount(2, std::chrono::seconds(5)))
        << "both metadata items should be sent";

    // Set agent to exiting state before stopping worker
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();
    if (meta_worker.joinable()) {
        meta_worker.join();
    }

    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::API), 1u);
    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::STRING), 1u);
    EXPECT_EQ(fake->apiRequest(0).apiinfo(), "test.api");
    EXPECT_EQ(fake->stringRequest(0).stringvalue(), "test.string");
}

// GrpcStats Tests with Mock Stubs

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

TEST_F(GrpcMockTest, GrpcStatsWorkerContainsChannelSetupException) {
    GrpcClientTuning tuning;
    tuning.worker_restart_delay = std::chrono::milliseconds(10);
    ThrowingReadyGrpcStats stats_client(mock_agent_service_->getConfig(), tuning);
    stats_client.setAgentService(mock_agent_service_.get());

    // The worker must contain the exception and keep retrying (supervised
    // restart) instead of dying — so it only returns once stopped.
    ScopedWorker stats_worker(
        [&stats_client] { stats_client.stopStatsWorker(); },
        [&stats_client] { EXPECT_NO_THROW(stats_client.sendStatsWorker()); });

    // Two attempts distinguish a supervised restart from a worker that died
    // permanently after the first injected exception (which a plain sleep
    // plus no-throw check could not tell apart).
    EXPECT_TRUE(wait_for_condition(
        [&stats_client] { return stats_client.attempts() >= 2; },
        std::chrono::seconds(3)))
        << "the stats worker must restart after a thrown readiness check";

    stats_client.stopStatsWorker();
    if (stats_worker.joinable()) stats_worker.join();
    EXPECT_FALSE(mock_agent_service_->isExiting());
}

// ============================================================
// Workers on a dead channel
// ============================================================

// With readyChannel() false every worker must start, leave its stub
// untouched, and stop promptly even with items queued — the mock stubs
// cannot carry async streaming, so clean start/stop is the whole contract
// testable here. Delivery itself is covered by the FakeStub worker tests.
TEST_F(GrpcMockTest, GrpcWorkersStartAndStopCleanlyOnDeadChannelTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());
    TestableGrpcSpan span_client(mock_agent_service_.get());
    TestableGrpcStats stats_client(mock_agent_service_.get());
    agent.setReadyChannel(false);
    span_client.setReadyChannel(false);
    stats_client.setReadyChannel(false);

    agent.setMockAgentStub(std::make_unique<NiceMock<v1::MockAgentStub>>());
    agent.setMockMetaStub(std::make_unique<NiceMock<v1::MockMetadataStub>>());
    span_client.setMockSpanStub(std::make_unique<NiceMock<v1::MockSpanStub>>());
    stats_client.setMockStatsStub(std::make_unique<NiceMock<v1::MockStatStub>>());

    agent.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "dead.channel"));
    auto span_data = make_test_span_data_ptr(*mock_agent_service_, "dead-channel-op");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data, true));
    stats_client.enqueueStats(AGENT_STATS);

    ScopedWorker ping_worker([&agent] { agent.stopPingWorker(); },
                     [&agent] { agent.sendPingWorker(); });
    ScopedWorker meta_worker([&agent] { agent.stopMetaWorker(); },
                     [&agent] { agent.sendMetaWorker(); });
    ScopedWorker span_worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });
    ScopedWorker stats_worker([&stats_client] { stats_client.stopStatsWorker(); },
                     [&stats_client] { stats_client.sendStatsWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    mock_agent_service_->setExiting(true);
    agent.stopPingWorker();
    agent.stopMetaWorker();
    span_client.stopSpanWorker();
    stats_client.stopStatsWorker();

    if (ping_worker.joinable()) ping_worker.join();
    if (meta_worker.joinable()) meta_worker.join();
    if (span_worker.joinable()) span_worker.join();
    if (stats_worker.joinable()) stats_worker.join();
}

// ============================================================
// Mixed metadata success/failure in a single worker run
// ============================================================

TEST_F(GrpcMockTest, GrpcAgentMetaWorkerMixedSuccessFailureTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    // First API meta succeeds, second fails, third succeeds (launch order
    // follows queue order even though the sends are pipelined).
    fake->pushReply(FakeMetadataStub::MetaRpc::API, grpc::Status::OK, true);
    fake->pushReply(FakeMetadataStub::MetaRpc::API,
                    grpc::Status(grpc::StatusCode::UNAVAILABLE, "unavailable"), false);
    fake->pushReply(FakeMetadataStub::MetaRpc::API, grpc::Status::OK, true);

    agent.setMockMetaStub(std::move(fake_meta_stub));

    agent.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api.ok"));
    agent.enqueueMeta(std::make_unique<MetaData>(META_API, 2, 100, "api.fail"));
    agent.enqueueMeta(std::make_unique<MetaData>(META_API, 3, 100, "api.recover"));

    ScopedWorker meta_worker([&agent] { agent.stopMetaWorker(); },
                     [&agent] { agent.sendMetaWorker(); });

    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::API, 3, std::chrono::seconds(5)))
        << "the worker must keep processing items after a failure";
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(fake->apiRequest(0).apiinfo(), "api.ok");
    EXPECT_EQ(fake->apiRequest(1).apiinfo(), "api.fail");
    EXPECT_EQ(fake->apiRequest(2).apiinfo(), "api.recover");
}

TEST_F(GrpcMockTest, GrpcMetadataRetriesFailedResultWithoutEvictingCache) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(std::chrono::milliseconds(50));

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    // PResult.success=false on the first attempt, success on the retry.
    fake->pushReply(FakeMetadataStub::MetaRpc::API, grpc::Status::OK, false);
    fake->pushReply(FakeMetadataStub::MetaRpc::API, grpc::Status::OK, true);

    metadata.setMockMetaStub(std::move(fake_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api.retry"));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    // PResult.success=false must be retried after the (shrunk) retry delay
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::API, 2, std::chrono::seconds(5)));

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(mock_agent_service_->removed_api_count_, 0);
}

TEST_F(GrpcMockTest, GrpcMetadataRetriesItemWhenSendThrows) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(std::chrono::milliseconds(50));

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    // The async launch throws synchronously on the first attempt; the item
    // must re-enter the retry path (with its permit reclaimed) and succeed.
    fake->pushThrow(FakeMetadataStub::MetaRpc::API);
    fake->pushReply(FakeMetadataStub::MetaRpc::API, grpc::Status::OK, true);

    metadata.setMockMetaStub(std::move(fake_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api.throw.retry"));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::API, 2, std::chrono::seconds(5)));

    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::API), 2u);
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

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();
}

TEST_F(GrpcMockTest, GrpcMetadataEvictsCacheAfterRetryExhaustion) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(std::chrono::milliseconds(50));

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    fake->setReplyMode(FakeMetadataStub::ReplyMode::ERROR_STATUS);

    metadata.setMockMetaStub(std::move(fake_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api.exhaust"));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    // Eviction happens only after 3 scheduled retries are exhausted
    EXPECT_TRUE(wait_for_condition(
        [this] { return mock_agent_service_->removed_api_count_ >= 1; }, std::chrono::seconds(10)));

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(mock_agent_service_->removed_api_count_, 1);
    // Initial send + exactly 3 scheduled retries.
    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::API), 4u);
}

TEST_F(GrpcMockTest, GrpcMetadataEvictsErrorCacheAfterRetryExhaustion) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(std::chrono::milliseconds(50));

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    fake->setReplyMode(FakeMetadataStub::ReplyMode::ERROR_STATUS);

    metadata.setMockMetaStub(std::move(fake_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(
        META_STRING, 2, "error.exhaust", STRING_META_ERROR));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    EXPECT_TRUE(wait_for_condition(
        [this] { return mock_agent_service_->removed_error_count_.load() >= 1; },
        std::chrono::seconds(10)));

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(mock_agent_service_->removed_error_count_.load(), 1);
    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::STRING), 4u);
}

TEST_F(GrpcMockTest, GrpcMetadataEvictsSqlCacheAfterRetryExhaustion) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(std::chrono::milliseconds(50));

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    fake->setReplyMode(FakeMetadataStub::ReplyMode::ERROR_STATUS);

    metadata.setMockMetaStub(std::move(fake_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(
        META_STRING, 3, "SELECT exhaust", STRING_META_SQL));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    EXPECT_TRUE(wait_for_condition(
        [this] { return mock_agent_service_->removed_sql_count_.load() >= 1; },
        std::chrono::seconds(10)));

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(mock_agent_service_->removed_sql_count_.load(), 1);
    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::SQL), 4u);
}

TEST_F(GrpcMockTest, GrpcMetadataEvictsSqlUidCacheAfterRetryExhaustion) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(std::chrono::milliseconds(50));

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    fake->setReplyMode(FakeMetadataStub::ReplyMode::ERROR_STATUS);

    metadata.setMockMetaStub(std::move(fake_meta_stub));
    const SqlUid uid{0, 1, 2, 3, 4, 5, 6, 7,
                     8, 9, 10, 11, 12, 13, 14, 15};
    metadata.enqueueMeta(std::make_unique<MetaData>(
        META_SQL_UID, uid, "SELECT uid_exhaust"));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    EXPECT_TRUE(wait_for_condition(
        [this] { return mock_agent_service_->removed_sql_uid_count_.load() >= 1; },
        std::chrono::seconds(10)));

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(mock_agent_service_->removed_sql_uid_count_.load(), 1);
    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::SQL_UID), 4u);
}

// ============================================================
// All metadata types sent successfully via worker
// ============================================================

TEST_F(GrpcMockTest, GrpcAgentMetaWorkerAllTypesSuccessTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    agent.setMockMetaStub(std::move(fake_meta_stub));

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

    ScopedWorker meta_worker([&agent] { agent.stopMetaWorker(); },
                     [&agent] { agent.sendMetaWorker(); });

    EXPECT_TRUE(fake->waitForTotalRequestCount(5, std::chrono::seconds(5)));
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::API), 1u);
    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::STRING), 1u);
    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::SQL), 1u);
    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::SQL_UID), 1u);
    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::EXCEPTION), 1u);
}

// ============================================================
// All metadata types fail via worker
// ============================================================

TEST_F(GrpcMockTest, GrpcAgentMetaWorkerAllTypesFailureTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    fake->setReplyMode(FakeMetadataStub::ReplyMode::ERROR_STATUS);
    agent.setMockMetaStub(std::move(fake_meta_stub));

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

    ScopedWorker meta_worker([&agent] { agent.stopMetaWorker(); },
                     [&agent] { agent.sendMetaWorker(); });

    EXPECT_TRUE(fake->waitForTotalRequestCount(5, std::chrono::seconds(5)));
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
    ScopedWorker stats_worker([&stats_client] { stats_client.stopStatsWorker(); },
                     [&stats_client] { stats_client.sendStatsWorker(); });

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

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    agent.setMockMetaStub(std::move(fake_meta_stub));

    agent.enqueueMeta(std::make_unique<MetaData>(META_STRING, 1, "SELECT * FROM users", STRING_META_SQL));

    ScopedWorker meta_worker([&agent] { agent.stopMetaWorker(); },
                     [&agent] { agent.sendMetaWorker(); });

    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::SQL, 1, std::chrono::seconds(5)));
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(fake->sqlRequest(0).sqlid(), 1);
    EXPECT_EQ(fake->sqlRequest(0).sql(), "SELECT * FROM users");
}

TEST_F(GrpcMockTest, GrpcAgentSendSqlUidMetaSuccessTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    agent.setMockMetaStub(std::move(fake_meta_stub));

    SqlUid uid = {0xAA, 0xBB, 0xCC, 0xDD};
    agent.enqueueMeta(std::make_unique<MetaData>(META_SQL_UID, uid, "INSERT INTO t VALUES (?)"));

    ScopedWorker meta_worker([&agent] { agent.stopMetaWorker(); },
                     [&agent] { agent.sendMetaWorker(); });

    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::SQL_UID, 1, std::chrono::seconds(5)));
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(fake->sqlUidRequest(0).sql(), "INSERT INTO t VALUES (?)");
}

TEST_F(GrpcMockTest, GrpcAgentSendExceptionMetaSuccessTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    agent.setMockMetaStub(std::move(fake_meta_stub));

    TraceId txid{"test-agent", 12345, 1};
    std::vector<std::unique_ptr<Exception>> exceptions;
    auto cs = std::make_unique<CallStack>("NullPointerException");
    cs->push("libcore", "deref", "ptr.cpp", 42);
    cs->push("app", "main", "main.cpp", 100);
    exceptions.push_back(std::make_unique<Exception>(std::move(cs)));

    agent.enqueueMeta(std::make_unique<MetaData>(META_EXCEPTION, txid, 999, "/api/v2/resource", std::move(exceptions)));

    ScopedWorker meta_worker([&agent] { agent.stopMetaWorker(); },
                     [&agent] { agent.sendMetaWorker(); });

    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::EXCEPTION, 1, std::chrono::seconds(5)));
    mock_agent_service_->setExiting(true);
    agent.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    ASSERT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::EXCEPTION), 1u);
    EXPECT_EQ(fake->exceptionRequest(0).uritemplate(), "/api/v2/resource");
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

    ScopedWorker worker([&command] { command.stopCommandWorker(); },
                     [&command] { command.commandWorker(); });

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

    ScopedWorker worker([&command] { command.stopCommandWorker(); },
                     [&command] { command.commandWorker(); });

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

    ScopedWorker worker([&command] { command.stopCommandWorker(); },
                     [&command] { command.commandWorker(); });

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

    ScopedWorker worker([&command] { command.stopCommandWorker(); },
                     [&command] { command.commandWorker(); });

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

    std::atomic<bool> worker_done{false};
    ScopedWorker worker([&command] { command.stopCommandWorker(); },
                        [&command, &worker_done] {
                            command.commandWorker();
                            worker_done.store(true);
                        });

    // Bounded instead of an unconditional join: a regression that keeps the
    // worker retrying on a not-ready channel must turn this test red, not
    // hang it forever (the guard stops and joins the worker on scope exit).
    EXPECT_TRUE(wait_for_condition([&worker_done] { return worker_done.load(); },
                                   std::chrono::seconds(2)))
        << "Command worker should exit immediately when the channel is not ready";
}

TEST_F(GrpcMockTest, GrpcCommandStopWorkerWakesReconnectDelay) {
    // Inflated delays make the assertion below unambiguous: a woken stop
    // returns in milliseconds, a lost wakeup sleeps out five seconds.
    GrpcClientTuning tuning;
    tuning.worker_restart_delay = std::chrono::seconds(5);
    tuning.reconnect_initial_interval = std::chrono::seconds(5);
    TestableGrpcCommand command(mock_agent_service_.get(), tuning);

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

    ScopedWorker worker([&command] { command.stopCommandWorker(); },
                     [&command] { command.commandWorker(); });

    ASSERT_EQ(stream_opened.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);

    // The worker is now in (or heading into) the injected 5s reconnect
    // delay; stopCommandWorker must wake it so shutdown does not block.
    mock_agent_service_->setExiting(true);
    const auto stop_start = std::chrono::steady_clock::now();
    command.stopCommandWorker();
    if (worker.joinable()) worker.join();
    EXPECT_LT(std::chrono::steady_clock::now() - stop_start, std::chrono::seconds(2))
        << "stopCommandWorker must wake the reconnect delay, not sleep it out";
}

// ============================================================
// GrpcSpan SendSpanBatch tests (fake async stub)
// ============================================================

// enqueueSpan drops a chunk instead of queueing it: a null chunk would reach
// the worker and crash at getSpanData(), and a chunk handed over while the
// agent is exiting would sit in a queue nothing drains. Both drops must leave
// the queue usable, so a later chunk is still delivered.
TEST_F(GrpcMockTest, GrpcSpanEnqueueDropsNullAndExitingChunksTest) {
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.span_batch.size = 1;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 100;
    cfg->collector.span_batch.max_concurrent_requests = 2;

    TestableGrpcSpan span_client(mock_agent_service_.get());
    auto fake_stub = std::make_unique<FakeSpanStub>();
    auto* fake = fake_stub.get();
    span_client.setMockSpanStub(std::move(fake_stub));

    mock_agent_service_->setExiting(true);
    auto exiting_span = make_test_span_data_ptr(*mock_agent_service_, "exiting-op");
    exiting_span->setSpanId(1001);
    span_client.enqueueSpan(std::make_unique<SpanChunk>(exiting_span, true));
    mock_agent_service_->setExiting(false);

    span_client.enqueueSpan(nullptr);

    auto delivered_span = make_test_span_data_ptr(*mock_agent_service_, "delivered-op");
    delivered_span->setSpanId(1002);
    span_client.enqueueSpan(std::make_unique<SpanChunk>(delivered_span, true));

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });

    ASSERT_TRUE(fake->waitForBatchCount(1, std::chrono::seconds(2)))
        << "the chunk enqueued while the agent was running must be delivered";

    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();

    // Both enqueues run on this thread, so they share a queue shard and keep
    // FIFO order: a retained exiting chunk would have been sent first.
    EXPECT_EQ(fake->batchCount(), 1u)
        << "only the accepted chunk should reach the collector";
    const auto request = fake->request(0);
    ASSERT_EQ(request.span_size(), 1);
    ASSERT_TRUE(request.span(0).has_span());
    EXPECT_EQ(request.span(0).span().spanid(), 1002)
        << "the chunk enqueued while exiting must be dropped, not queued";
}

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

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });

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

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });

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

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });

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
    span_data->getAnnotations()->AppendData(103, AnnotationData("string-value"));
    span_data->getAnnotations()->AppendStringString(104, "left", "right");
    span_data->getAnnotations()->AppendData(105, AnnotationData(7, "method", "GET"));
    span_data->getAnnotations()->AppendLongIntIntByteByteString(106, 99, 1, 2, 3, 4, "rpc");
    span_data->getAnnotations()->AppendData(107, AnnotationData(uid, "sql", "args"));
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data, true));

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });

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
    span_data->finishSpanEvent(span_data->topSpanEvent());
    span_client.enqueueSpan(std::make_unique<SpanChunk>(span_data, true));

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });

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

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });

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

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });

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

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });

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

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });

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

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });

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

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });

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

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });

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
    ScopedWorker boot([&grpc_agent] { grpc_agent.stopAgentInfo(); },
                      [&] { boot_result = grpc_agent.registerAgentWithRetry(); });

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

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });

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
    std::atomic<bool> worker_done{false};
    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                        [&span_client, &worker_done] {
                            span_client.sendSpanWorker();
                            worker_done.store(true);
                        });
    // Bounded instead of an unconditional join: if the shutdown flush
    // regresses to blocking on the dead collector — the exact behavior this
    // test pins — the test must go red on the elapsed assertion, not hang
    // before reaching it (the guard stops and joins on scope exit).
    const bool finished = wait_for_condition(
        [&worker_done] { return worker_done.load(); }, std::chrono::seconds(3));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (finished && worker.joinable()) worker.join();

    EXPECT_EQ(fake->batchCount(), 0u)
        << "remaining spans must be dropped, not sent, while the channel is down";
    EXPECT_TRUE(finished) << "the shutdown flush must finish, not block indefinitely";
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

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();

    metadata.setMockMetaStub(std::move(fake_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api.outage.recovery"));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::API, 1, std::chrono::seconds(5)))
        << "metadata should be re-sent once the channel recovers";

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::API), 1u);
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

    ScopedWorker worker([&command] { command.stopCommandWorker(); },
                     [&command] { command.commandWorker(); });

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

    ScopedWorker ping_worker([&grpc_agent] { grpc_agent.stopPingWorker(); },
                     [&grpc_agent] { grpc_agent.sendPingWorker(); });

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

    ScopedWorker stats_worker([&stats_client] { stats_client.stopStatsWorker(); },
                     [&stats_client] { stats_client.sendStatsWorker(); });

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

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    metadata.setMockMetaStub(std::move(fake_meta_stub));

    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "overflow.api.1"));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 2, 100, "overflow.api.2"));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 3, 100, "overflow.api.3"));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    // Only the 2 queued metas may be sent; the third was dropped on enqueue.
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::API, 2, std::chrono::seconds(5)));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::API), 2u);
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

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    fake->setReplyMode(FakeMetadataStub::ReplyMode::ERROR_STATUS);

    metadata.setMockMetaStub(std::move(fake_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api.injected.retry"));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    EXPECT_TRUE(wait_for_condition(
        [this] { return mock_agent_service_->removed_api_count_ >= 1; }, std::chrono::seconds(5)))
        << "the cache entry must be released after the injected retry budget is exhausted";

    // Give an unexpected extra retry the chance to fire before stopping, so
    // the count assertion below would catch it.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    // Initial send + exactly one scheduled retry (instead of the default 3)
    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::API), 2u);
    EXPECT_EQ(mock_agent_service_->removed_api_count_, 1);
}

// ============================================================
// Metadata pipelining: multiple RPCs in flight behind the permit cap
// ============================================================

TEST_F(GrpcMockTest, GrpcMetadataPipelinesSendsUpToPermitCap) {
    GrpcClientTuning tuning;
    tuning.meta_max_concurrent_requests = 2;
    TestableGrpcMetadata metadata(mock_agent_service_.get(), tuning);

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    // Hold every completion: in-flight RPCs stay open until released.
    fake->setReplyMode(FakeMetadataStub::ReplyMode::HOLD);

    metadata.setMockMetaStub(std::move(fake_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "pipeline.1"));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 2, 100, "pipeline.2"));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 3, 100, "pipeline.3"));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    // Both permits go in flight without waiting for either completion — the
    // serial-sender behavior this pipeline replaced allowed only one.
    EXPECT_TRUE(fake->waitForHeldCallbacks(2, std::chrono::seconds(5)))
        << "two sends must be in flight concurrently";
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::API), 2u)
        << "the third send must wait for a free permit";

    // Completing one in-flight call frees its permit for the third item.
    EXPECT_TRUE(fake->releaseHeldCallback(0, grpc::Status::OK));
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::API, 3, std::chrono::seconds(5)))
        << "a released permit must admit the queued item";

    fake->releaseHeldCallbacks(grpc::Status::OK);
    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(mock_agent_service_->removed_api_count_, 0);
}

TEST_F(GrpcMockTest, GrpcMetadataSurvivesReadyChannelExceptionWithoutLeakingPermit) {
    GrpcClientTuning tuning;
    // A single permit: if the throwing readiness check leaked it, no later
    // item could ever launch and the waits below would time out.
    tuning.meta_max_concurrent_requests = 1;
    tuning.meta_retry_delay = std::chrono::milliseconds(50);
    TestableGrpcMetadata metadata(mock_agent_service_.get(), tuning);
    // The first readiness check (permit already held) throws; the retry
    // must find the channel healthy again.
    metadata.setReadyChannelThrows(1);

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    metadata.setMockMetaStub(std::move(fake_meta_stub));

    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api.ready.throw"));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    // The item survives the exception on the retry path and is delivered.
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::API, 1, std::chrono::seconds(5)))
        << "the item dequeued before the throwing readiness check must not be lost";

    // The permit survives too: a second item can still launch.
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 2, 100, "api.after.throw"));
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::API, 2, std::chrono::seconds(5)))
        << "the single permit must have been handed back after the exception";

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(mock_agent_service_->removed_api_count_, 0)
        << "a transient readiness exception must not strand or evict the cache entry";
}

TEST_F(GrpcMockTest, GrpcMetadataCompletionSurvivesAllocationPressureWithoutLosingWakeup) {
    GrpcClientTuning tuning;
    // A single permit and a queued second item: if the completion's notify
    // were lost, the worker would stay parked in its bare cv.wait() with a
    // free permit and the waits below would time out.
    tuning.meta_max_concurrent_requests = 1;
    tuning.meta_retry_delay = std::chrono::milliseconds(50);
    TestableGrpcMetadata metadata(mock_agent_service_.get(), tuning);

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    fake->setReplyMode(FakeMetadataStub::ReplyMode::HOLD);
    metadata.setMockMetaStub(std::move(fake_meta_stub));

    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "api.alloc.fail"));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 2, 100, "api.alloc.after"));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    ASSERT_TRUE(fake->waitForHeldCallbacks(1, std::chrono::seconds(5)))
        << "the first item must be in flight holding the single permit";

    // A failed status whose message outgrows any SSO buffer: completing with
    // it makes every string copy on the completion path allocate. Built
    // before arming, so its own allocations run unrestricted.
    grpc::Status failed_status(grpc::StatusCode::UNAVAILABLE, std::string(192, 'x'));

    // The completion path runs inline on this thread. With the single-shot
    // failure armed, any allocation between the permit release and the
    // notify throws bad_alloc — the regression this pins: a copy assignment
    // of the status there threw, gRPC's callback layer swallowed it, and
    // the worker stayed parked while the item's outcome was lost.
    arm_allocation_failure();
    bool released = false;
    bool threw = false;
    try {
        released = fake->releaseHeldCallbackByMove(0, std::move(failed_status));
    } catch (...) {
        threw = true;
    }
    const bool allocated_in_completion = clear_allocation_failure();
    EXPECT_FALSE(threw) << "the completion path must not throw";
    EXPECT_FALSE(allocated_in_completion)
        << "the completion path must not allocate on the callback thread";
    // A throw above happened inside the invoked callback, so either way the
    // held callback existed; neither means the test setup is broken.
    ASSERT_TRUE(released || threw) << "the held completion callback must exist";

    // The worker saw the completion: the queued second item launches on the
    // returned permit.
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::API, 2, std::chrono::seconds(5)))
        << "the returned permit and the completion notify must wake the worker for the queued item";

    // Free the second item's permit so the failed first item's retry can
    // launch: its outcome must have been recorded, not dropped.
    EXPECT_TRUE(fake->waitForHeldCallbacks(1, std::chrono::seconds(5)));
    fake->releaseHeldCallbacks(grpc::Status::OK);
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::API, 3, std::chrono::seconds(5)))
        << "the failed completion's outcome must reach the worker and schedule a retry";

    fake->releaseHeldCallbacks(grpc::Status::OK);
    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(mock_agent_service_->removed_api_count_, 0)
        << "a completion under allocation pressure must not strand or evict the cache entry";
}

TEST_F(GrpcMockTest, GrpcMetadataShutdownAwaitForInFlightIsBounded) {
    GrpcClientTuning tuning;
    tuning.meta_shutdown_await_timeout = std::chrono::milliseconds(50);
    TestableGrpcMetadata metadata(mock_agent_service_.get(), tuning);

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    // The in-flight call never completes (a stalled collector that also
    // ignores TryCancel): shutdown must still finish within the bounded
    // await instead of hanging on the outstanding permit.
    fake->setReplyMode(FakeMetadataStub::ReplyMode::HOLD);

    metadata.setMockMetaStub(std::move(fake_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(META_API, 1, 100, "shutdown.hold"));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });
    ASSERT_TRUE(fake->waitForHeldCallbacks(1, std::chrono::seconds(5)));

    const auto stop_start = std::chrono::steady_clock::now();
    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();
    EXPECT_LT(std::chrono::steady_clock::now() - stop_start, std::chrono::seconds(2))
        << "shutdown must not wait unbounded for a stalled in-flight metadata call";

    // A late completion after the worker is gone must stay safe: the
    // callback touches only the shared pipeline state.
    fake->releaseHeldCallbacks(grpc::Status::OK);
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

    ScopedWorker worker([&command] { command.stopCommandWorker(); },
                     [&command] { command.commandWorker(); });

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
                    // Bounded so a test path that never releases the park
                    // cannot leave this stream thread unjoinable forever.
                    released.wait_for(std::chrono::seconds(10));
                    return false;
                }))
                .WillRepeatedly(Return(false));
            ON_CALL(*writer, WritesDone()).WillByDefault(Return(true));
            ON_CALL(*writer, Finish()).WillByDefault(Return(grpc::Status::OK));
            return writer;
        }));

    command.setMockCommandStub(std::move(mock_command_stub));

    ScopedWorker worker([&command] { command.stopCommandWorker(); },
                     [&command] { command.commandWorker(); });

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

TEST_F(GrpcMockTest, ReissuedActiveThreadCountRequestDoesNotJoinBlockedPredecessor) {
    GrpcClientTuning tuning;
    tuning.active_thread_count_flush_interval = std::chrono::milliseconds(20);
    tuning.max_active_thread_count_streams = 4;
    TestableGrpcCommand command(mock_agent_service_.get(), tuning);

    auto mock_command_stub = std::make_unique<NiceMock<v1::MockProfilerCommandServiceStub>>();

    auto make_atc_request = [](int32_t request_id) {
        v1::PCmdRequest request;
        request.set_requestid(request_id);
        request.mutable_commandactivethreadcount();
        return request;
    };

    // The first stream simulates a collector that stalls mid-stream and does
    // not honor cancellation: its Write parks until the test releases it.
    std::promise<void> write_entered;
    std::shared_future<void> write_entered_seen = write_entered.get_future().share();
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();

    auto* stream = new NiceMock<MockCmdStream>();
    EXPECT_CALL(*stream, Read(_))
        .WillOnce(DoAll(SetArgPointee<0>(make_atc_request(301)), Return(true)))
        .WillOnce(Invoke([make_atc_request, write_entered_seen](v1::PCmdRequest* request) {
            // Deliver the duplicate only once the first stream is provably
            // parked in Write, so the handler faces a live, uncancellable
            // predecessor for the same request id. Bounded: if the
            // predecessor never parks (a production regression), end the
            // command stream instead of wedging the worker inside this mock
            // forever — the test then fails its wait below and still joins.
            if (write_entered_seen.wait_for(std::chrono::seconds(5)) !=
                std::future_status::ready) {
                return false;
            }
            *request = make_atc_request(301);
            return true;
        }))
        .WillRepeatedly(Return(false));

    EXPECT_CALL(*mock_command_stub, HandleCommandV2Raw(_))
        .WillOnce(Return(stream))
        .WillRepeatedly(Invoke(make_idle_cmd_stream));

    std::promise<void> second_stream_started;
    EXPECT_CALL(*mock_command_stub, CommandStreamActiveThreadCountRaw(_, _))
        .Times(2)
        .WillOnce(InvokeWithoutArgs([&write_entered, released] {
            auto* writer = new NiceMock<MockActiveThreadCountWriter>();
            EXPECT_CALL(*writer, Write(_, _))
                .WillOnce(InvokeWithoutArgs([&write_entered, released] {
                    write_entered.set_value();
                    // Bounded so a test path that never releases the park
                    // cannot leave this stream thread unjoinable forever.
                    released.wait_for(std::chrono::seconds(10));
                    return false;
                }))
                .WillRepeatedly(Return(false));
            ON_CALL(*writer, WritesDone()).WillByDefault(Return(true));
            ON_CALL(*writer, Finish()).WillByDefault(Return(grpc::Status::OK));
            return writer;
        }))
        .WillOnce(InvokeWithoutArgs([&second_stream_started] {
            // The replacement stream ends immediately: the test only needs
            // its RPC to start, which proves the handler got past the wedged
            // predecessor without joining it.
            auto* writer = new NiceMock<MockActiveThreadCountWriter>();
            ON_CALL(*writer, Write(_, _)).WillByDefault(Return(false));
            ON_CALL(*writer, WritesDone()).WillByDefault(Return(true));
            ON_CALL(*writer, Finish()).WillByDefault(Return(grpc::Status::OK));
            second_stream_started.set_value();
            return writer;
        }));

    command.setMockCommandStub(std::move(mock_command_stub));

    ScopedWorker worker([&command] { command.stopCommandWorker(); },
                     [&command] { command.commandWorker(); });

    // Before the fix, the handler joined the parked predecessor under
    // active_streams_mutex_ and the replacement stream could never start.
    EXPECT_EQ(second_stream_started.get_future().wait_for(std::chrono::seconds(5)),
              std::future_status::ready)
        << "the re-issued request must start its stream without joining the blocked predecessor";

    // And the registry mutex stays free while the predecessor is wedged, so
    // the shutdown signal phase cannot block behind the handler either.
    auto request_stop_done = std::async(std::launch::async, [&command] {
        command.requestStopCommandWorker();
    });
    EXPECT_EQ(request_stop_done.wait_for(std::chrono::seconds(2)), std::future_status::ready)
        << "the signal phase must not wait behind a join of the blocked stream";

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

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                     [&span_client] { span_client.sendSpanWorker(); });

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

    ScopedWorker ping_worker([&agent] { agent.stopPingWorker(); },
                             [&agent] { EXPECT_NO_THROW(agent.sendPingWorker()); });

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

    ScopedWorker worker([&command] { command.stopCommandWorker(); },
                     [&command] { command.commandWorker(); });

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
