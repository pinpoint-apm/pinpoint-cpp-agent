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
#include <filesystem>
#include <fstream>
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
#include "../src/grpc_builders.h"
#include "../src/logging.h"
#include "../src/agent_service.h"
#include "../src/config.h"
#include "../src/span.h"
#include "../src/stat.h"
#include "../src/url_stat.h"
#include "../src/utility.h"
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

    // Status returned by ReplyMode::ERROR_STATUS (default UNAVAILABLE), so a
    // test can hold one code for an unbounded number of attempts.
    void setErrorStatus(grpc::Status status) {
        std::unique_lock<std::mutex> lock(mutex_);
        error_status_ = std::move(status);
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
                        status = error_status_;
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
    grpc::Status error_status_{grpc::StatusCode::UNAVAILABLE, "fake unavailable"};
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
// Mirrors GrpcClient::readyChannel()'s contract for a mock: block while the
// injected channel state is "down", return true once it is up and false only
// when the client is stopping first.
template <typename Client>
static bool wait_ready_or_stopping(Client& client, const std::atomic<bool>& ready) {
    while (!ready.load()) {
        if (client.stoppingForTest()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return !client.stoppingForTest();
}

class TestableGrpcMetadata : public GrpcMetadata {
public:
    explicit TestableGrpcMetadata(AgentService* agent, const GrpcClientTuning& tuning = {})
        : GrpcMetadata(agent->getConfig(), tuning) {
        setAgentService(agent);
    }

    void setMockMetaStub(std::unique_ptr<v1::Metadata::StubInterface> mock_stub) {
        set_meta_stub(std::move(mock_stub));
    }
    bool stoppingForTest() const { return stopping(); }

    // The non-waiting probe the metadata pipeline uses (see
    // GrpcClient::channelReadyNow): false while the channel is down.
    bool channelReadyNow() override {
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

    // Models the production contract: waits out the outage and returns
    // false only once the client is stopping. If the metadata worker ever
    // goes back to calling this while holding a permit and an item, the
    // outage tests below stall and fail instead of passing by accident.
    bool readyChannel() override { return wait_ready_or_stopping(*this, ready_channel_); }

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
    bool stoppingForTest() const { return stopping(); }

    // Production readyChannel() blocks through a collector outage and returns
    // false only once the client is stopping (or no channel was opened); the
    // mock does the same so outage tests exercise the real worker behavior.
    bool readyChannel() override { return wait_ready_or_stopping(*this, ready_channel_); }
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
    std::vector<StatsType> queuedStatsForTest() {
        std::lock_guard<std::mutex> lock(stats_queue_mutex_);
        return stats_queue_;
    }
    GrpcStreamStatus nextWriteForTest() { return next_write(); }

    // The shutdown URL-stat flush. stats_channel_ready() probes a live
    // grpc::Channel's connectivity state, which no unit test has, so it is
    // driven from here instead; the rest of the flush is the production code.
    bool stats_channel_ready() const override { return stats_channel_ready_; }
    void setStatsChannelReady(bool ready) { stats_channel_ready_ = ready; }
    GrpcStreamStatus buildShutdownFlushForTest() { return build_shutdown_url_stat_write(); }
    void flushUrlStatsOnShutdownForTest() { flush_url_stats_on_shutdown(); }
    const v1::PStatMessage* pendingMessageForTest() const { return pending_message(); }
    uint64_t shutdownDroppedUrlStatsForTest() const { return shutdown_dropped_url_stats(); }

private:
    // Toggled by the test body while the worker thread polls readyChannel();
    // atomic so TSan-clean, like ready_channel_failures_.
    std::atomic<bool> ready_channel_{true};
    std::atomic<bool> stats_channel_ready_{true};
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
    CountingAgentInfoGrpcAgent(std::shared_ptr<const Config> config, GrpcRequestStatus result,
                               const GrpcClientTuning& tuning = {})
        : GrpcAgent(std::move(config), tuning), result_(result) {}

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
        cfg->http.url_stat.trim_path_depth = 4;
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

namespace {
    // registerAgent() only accepts a registration the collector acknowledged in
    // the response body, so every "collector accepted it" stub must fill the
    // out-param — a default-constructed PResult means success=false.
    v1::PResult accepted_result() {
        v1::PResult result;
        result.set_success(true);
        return result;
    }

    v1::PResult rejected_result(const std::string& message) {
        v1::PResult result;
        result.set_success(false);
        result.set_message(message);
        return result;
    }
}

TEST_F(GrpcMockTest, GrpcAgentRegisterAgentSuccessTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());
    
    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    
    // Set up expectation for successful agent registration
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(DoAll(SetArgPointee<2>(accepted_result()), Return(grpc::Status::OK)));
    
    agent.setMockAgentStub(std::move(mock_agent_stub));
    
    GrpcRequestStatus status = agent.registerAgent();
    
    EXPECT_EQ(status, SEND_OK) << "Agent registration should succeed with mock stub";
}

TEST_F(GrpcMockTest, GrpcAgentRegisterAgentUsesDefaultServerMetaData) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    v1::PAgentInfo captured_agent_info;
    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(DoAll(SaveArg<1>(&captured_agent_info),
                        SetArgPointee<2>(accepted_result()), Return(grpc::Status::OK)));

    agent.setMockAgentStub(std::move(mock_agent_stub));

    EXPECT_EQ(agent.registerAgent(), SEND_OK);

    ASSERT_TRUE(captured_agent_info.has_servermetadata());
    const auto& server_metadata = captured_agent_info.servermetadata();
    EXPECT_EQ(server_metadata.serverinfo(), "C/C++ Application");
    EXPECT_EQ(server_metadata.vmarg_size(), 0);
    EXPECT_EQ(server_metadata.serviceinfo_size(), 0);
}

TEST_F(GrpcMockTest, GrpcAgentRegisterAgentReportsReloadedConfig) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    std::vector<v1::PAgentInfo> captured;
    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .Times(2)
        .WillRepeatedly(DoAll(
            Invoke([&captured](grpc::ClientContext*, const v1::PAgentInfo& info, v1::PResult*) {
                captured.push_back(info);
            }),
            SetArgPointee<2>(accepted_result()), Return(grpc::Status::OK)));
    agent.setMockAgentStub(std::move(mock_agent_stub));

    EXPECT_EQ(agent.registerAgent(), SEND_OK);

    mock_agent_service_->publishConfig([](Config& c) {
        c.sampling.percent_rate = 12.5;
        c.is_container = true;
    });

    EXPECT_EQ(agent.registerAgent(), SEND_OK);
    ASSERT_EQ(captured.size(), 2U);

    EXPECT_FALSE(captured[0].container());
    EXPECT_TRUE(captured[1].container());
}

TEST_F(GrpcMockTest, GrpcAgentRegisterAgentRacesConfigReload) {
    // Sanitizer target: registerAgent() on one thread while reloads publish
    // new config snapshots on another. Each build_agent_info() must read one
    // consistent snapshot without touching the swapped pointer unsynchronized.
    TestableGrpcAgent agent(mock_agent_service_.get());
    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    ON_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillByDefault(DoAll(SetArgPointee<2>(accepted_result()), Return(grpc::Status::OK)));
    agent.setMockAgentStub(std::move(mock_agent_stub));

    std::atomic<bool> stop{false};
    std::thread reloader([&] {
        for (int i = 0; !stop.load(); ++i) {
            mock_agent_service_->publishConfig([i](Config& c) {
                c.sampling.percent_rate = static_cast<double>(i % 100);
                c.is_container = (i % 2) == 0;
            });
        }
    });
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(agent.registerAgent(), SEND_OK);
    }
    stop.store(true);
    reloader.join();
}

TEST_F(GrpcMockTest, GrpcAgentRegisterAgentUsesServerMetaData) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    v1::PAgentInfo captured_agent_info;
    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(DoAll(SaveArg<1>(&captured_agent_info),
                        SetArgPointee<2>(accepted_result()), Return(grpc::Status::OK)));

    agent.setMockAgentStub(std::move(mock_agent_stub));
    agent.setServerMetaData("test-server", {"--port=8080", "--worker=4"}, {"libfoo.so", "libbar.so"});

    EXPECT_EQ(agent.registerAgent(), SEND_OK);

    ASSERT_TRUE(captured_agent_info.has_servermetadata());
    const auto& server_metadata = captured_agent_info.servermetadata();
    EXPECT_EQ(server_metadata.serverinfo(), "test-server");
    ASSERT_EQ(server_metadata.vmarg_size(), 2);
    EXPECT_EQ(server_metadata.vmarg(0), "--port=8080");
    EXPECT_EQ(server_metadata.vmarg(1), "--worker=4");

    ASSERT_EQ(server_metadata.serviceinfo_size(), 1);
    const auto& service_info = server_metadata.serviceinfo(0);
    EXPECT_EQ(service_info.servicename(), "Libraries");
    ASSERT_EQ(service_info.servicelib_size(), 2);
    EXPECT_EQ(service_info.servicelib(0), "libfoo.so");
    EXPECT_EQ(service_info.servicelib(1), "libbar.so");
}

TEST_F(GrpcMockTest, GrpcAgentRegisterAgentSanitizesInvalidUtf8) {
    // ServerMetaData is caller-supplied (vm_args is usually raw argv). Invalid
    // UTF-8 in any of it makes the collector's readStringRequireUtf8 drop the
    // whole PAgentInfo, so the agent never registers and traces nothing --
    // permanently, since every retry resends the same bytes.
    TestableGrpcAgent agent(mock_agent_service_.get());

    v1::PAgentInfo captured;
    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(DoAll(SaveArg<1>(&captured),
                        SetArgPointee<2>(accepted_result()), Return(grpc::Status::OK)));
    agent.setMockAgentStub(std::move(mock_agent_stub));
    agent.setServerMetaData("caf\xe9-server", {"--name=\x80", "--ok=1"}, {"lib\xff.so"});

    EXPECT_EQ(agent.registerAgent(), SEND_OK);

    // toValidUtf8(x) == x is exactly "x is valid UTF-8" (see test_utility.cpp).
    std::string serialized;
    ASSERT_TRUE(captured.SerializeToString(&serialized));
    const auto& md = captured.servermetadata();
    const std::string fffd = "\xef\xbf\xbd";
    EXPECT_EQ(md.serverinfo(), "caf" + fffd + "-server");
    ASSERT_EQ(md.vmarg_size(), 2);
    EXPECT_EQ(md.vmarg(0), "--name=" + fffd);
    EXPECT_EQ(md.vmarg(1), "--ok=1");
    ASSERT_EQ(md.serviceinfo(0).servicelib_size(), 1);
    EXPECT_EQ(md.serviceinfo(0).servicelib(0), "lib" + fffd + ".so");
    EXPECT_EQ(toValidUtf8(captured.hostname()), captured.hostname());
    EXPECT_EQ(toValidUtf8(captured.ip()), captured.ip());
}

TEST_F(GrpcMockTest, GrpcAgentRegisterAgentKeepsValidNonAsciiIntact) {
    // Guards the other direction: sanitizing must not mangle legitimate
    // multi-byte text (Korean, CJK, emoji).
    TestableGrpcAgent agent(mock_agent_service_.get());

    v1::PAgentInfo captured;
    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(DoAll(SaveArg<1>(&captured),
                        SetArgPointee<2>(accepted_result()), Return(grpc::Status::OK)));
    agent.setMockAgentStub(std::move(mock_agent_stub));
    agent.setServerMetaData("서버-\xe6\x9d\xb1\xe4\xba\xac", {"--emoji=\xf0\x9f\x9a\x80", "--한글=값"},
                            {"라이브러리.so"});

    EXPECT_EQ(agent.registerAgent(), SEND_OK);

    const auto& md = captured.servermetadata();
    EXPECT_EQ(md.serverinfo(), "서버-\xe6\x9d\xb1\xe4\xba\xac");
    ASSERT_EQ(md.vmarg_size(), 2);
    EXPECT_EQ(md.vmarg(0), "--emoji=\xf0\x9f\x9a\x80");
    EXPECT_EQ(md.vmarg(1), "--한글=값");
    ASSERT_EQ(md.serviceinfo(0).servicelib_size(), 1);
    EXPECT_EQ(md.serviceinfo(0).servicelib(0), "라이브러리.so");
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

TEST_F(GrpcMockTest, GrpcAgentRegisterAgentRejectedByCollectorTest) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(DoAll(SetArgPointee<2>(rejected_result("unknown application name")),
                        Return(grpc::Status::OK)));
    agent.setMockAgentStub(std::move(mock_agent_stub));

    EXPECT_EQ(agent.registerAgent(), SEND_FAIL)
        << "a registration the collector rejected must not count as success";
}

// A rejection is retried like a transport failure — registration is the
// precondition for tracing, so the boot loop must keep going and must come up
// as soon as the collector changes its answer.
TEST_F(GrpcMockTest, GrpcAgentRegisterAgentRecoversAfterRejection) {
    TestableGrpcAgent agent(mock_agent_service_.get());

    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    EXPECT_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillOnce(DoAll(SetArgPointee<2>(rejected_result("collector busy")),
                        Return(grpc::Status::OK)))
        .WillOnce(DoAll(SetArgPointee<2>(accepted_result()), Return(grpc::Status::OK)));
    agent.setMockAgentStub(std::move(mock_agent_stub));

    EXPECT_EQ(agent.registerAgent(), SEND_FAIL);
    EXPECT_EQ(agent.registerAgent(), SEND_OK);
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
    auto api_meta = std::make_unique<MetaData>(ApiMeta(1, 100, "test.api"));
    agent.enqueueMeta(std::move(api_meta));

    auto str_meta = std::make_unique<MetaData>(StringMeta(2, "test.string", STRING_META_ERROR));
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

TEST_F(GrpcMockTest, GrpcStatsStallCoalescesTokensAndKeepsProducerData) {
    TestableGrpcStats stats_client(mock_agent_service_.get());

    mock_agent_service_->getAgentStats().incrSampleNew();
    UrlStatEntry url_stat{"/stalled", "GET", 200};
    url_stat.end_time_ = std::chrono::system_clock::now();
    url_stat.elapsed_ = 10;
    mock_agent_service_->getUrlStats().addSnapshot(&url_stat, *mock_agent_service_->getConfig());

    // Three collection cycles and two URL-stat ticks pass with no consumer.
    for (const auto stats : {AGENT_STATS, URL_STATS, AGENT_STATS, URL_STATS, AGENT_STATS}) {
        stats_client.enqueueStats(stats);
    }

    EXPECT_EQ(stats_client.queuedStatsForTest(), (std::vector<StatsType>{AGENT_STATS, URL_STATS}))
        << "one pending token per type, in first-enqueued order";

    AgentStatsSnapshot agent_snapshot;
    mock_agent_service_->getAgentStats().collectAgentStat(agent_snapshot);
    EXPECT_EQ(agent_snapshot.num_sample_new_, 1)
        << "a stalled stream must not reset the AgentStats counters";
    EXPECT_EQ(mock_agent_service_->getUrlStats().takeSnapshot(true)->getEachStats().size(), 1U)
        << "a stalled stream must not discard the URL snapshot";
}

// Pause publication after the new payload becomes visible but before its
// token is enqueued. The old token consumes that payload during the pause.
class DelayedStatTokenAgent : public MockAgentService {
public:
    TestableGrpcStats* sender{};
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    mutable int published{0};
    mutable bool released{false};

    void recordStats(StatsType type) const override {
        std::unique_lock<std::mutex> lock(mutex);
        ++published;
        cv.notify_all();
        if (published == 2) cv.wait(lock, [&] { return released; });
        sender->enqueueStats(type);
    }
};

TEST_F(GrpcMockTest, GrpcStatsDoesNotResendBatchWhenTokenRacesPublication) {
    DelayedStatTokenAgent agent;
    agent.mutableConfig()->stat.collect_interval = 10;
    agent.mutableConfig()->stat.batch_count = 1;
    TestableGrpcStats sender(&agent);
    agent.sender = &sender;
    auto& stats = agent.getAgentStats();
    std::thread worker([&] { stats.agentStatsWorker(); });
    bool second_published;
    {
        std::unique_lock<std::mutex> lock(agent.mutex);
        second_published = agent.cv.wait_for(lock, std::chrono::seconds(5),
                                             [&] { return agent.published == 2; });
    }
    EXPECT_TRUE(second_published);
    EXPECT_EQ(sender.nextWriteForTest(), STREAM_WRITE);
    sender.OnWriteDone(true);

    // Prevent a third publication and let the delayed second token through.
    agent.setExiting(true);
    {
        std::lock_guard<std::mutex> lock(agent.mutex);
        agent.released = true;
        agent.cv.notify_all();
    }
    stats.stopAgentStatsWorker();
    worker.join();
    agent.setExiting(false);
    EXPECT_EQ(sender.queuedStatsForTest(), (std::vector<StatsType>{AGENT_STATS}));
    EXPECT_EQ(sender.nextWriteForTest(), STREAM_CONTINUE)
        << "the second token's batch was already consumed by the first token";
    EXPECT_TRUE(sender.queuedStatsForTest().empty());
}

TEST_F(GrpcMockTest, GrpcStatsSendsNoMessageWhenNoUrlStatTickCompleted) {
    TestableGrpcStats stats_client(mock_agent_service_.get());

    stats_client.enqueueStats(URL_STATS);
    EXPECT_EQ(stats_client.nextWriteForTest(), STREAM_CONTINUE)
        << "an idle agent must not put an empty uri stat message on the stream";
    EXPECT_TRUE(stats_client.queuedStatsForTest().empty()) << "the token is still consumed";

    // A completed tick, on the other hand, is written. Two entries a tick
    // apart: the second cuts the first one's tick into the completed queue.
    auto& url_stats = mock_agent_service_->getUrlStats();
    const auto config = mock_agent_service_->getConfig();
    for (const int64_t second : {1000, 1030}) {
        UrlStatEntry entry{"/api/live", "GET", 200};
        entry.elapsed_ = 10;
        entry.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(second));
        url_stats.addSnapshot(&entry, *config);
    }

    stats_client.enqueueStats(URL_STATS);
    EXPECT_EQ(stats_client.nextWriteForTest(), STREAM_WRITE)
        << "a completed tick must still be written";
}

TEST_F(GrpcMockTest, GrpcStatsShutdownFlushSendsTickStillInProgress) {
    TestableGrpcStats stats_client(mock_agent_service_.get());
    auto& url_stats = mock_agent_service_->getUrlStats();
    const auto config = mock_agent_service_->getConfig();

    // A single entry: its tick is open, and nothing has been cut into
    // completed_ behind it.
    UrlStatEntry entry{"/api/shutdown", "GET", 200};
    entry.elapsed_ = 42;
    entry.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(1000));
    url_stats.addSnapshot(&entry, *config);

    // Steady state still sends nothing — the open tick may yet be cut whole.
    stats_client.enqueueStats(URL_STATS);
    EXPECT_EQ(stats_client.nextWriteForTest(), STREAM_CONTINUE)
        << "a periodic send must not split the tick in progress";

    mock_agent_service_->setExiting(true);
    stats_client.setStatsChannelReady(true);

    ASSERT_EQ(stats_client.buildShutdownFlushForTest(), STREAM_WRITE)
        << "the shutdown flush must write the tick in progress";
    const auto* msg = stats_client.pendingMessageForTest();
    ASSERT_NE(msg, nullptr);
    ASSERT_TRUE(msg->has_agenturistat());
    const auto& uri_stat = msg->agenturistat();
    ASSERT_EQ(uri_stat.eachuristat_size(), 1);
    EXPECT_EQ(uri_stat.eachuristat(0).uri(), "/api/shutdown");
    // 1000s bucketed into the 30s tick that starts at 990s.
    EXPECT_EQ(uri_stat.eachuristat(0).timestamp(), 990000);
    EXPECT_EQ(uri_stat.eachuristat(0).totalhistogram().total(), 42);
}

// The same flush also has to carry the completed ticks a stalled stream never
// drained (up to kMaxCompletedSnapshots of them), not just the open one:
// takeSnapshot merges completed_ into the message either way, and losing two
// minutes of ticks to a shutdown is the same bug at a larger scale.
TEST_F(GrpcMockTest, GrpcStatsShutdownFlushAlsoSendsRetainedCompletedTicks) {
    TestableGrpcStats stats_client(mock_agent_service_.get());
    auto& url_stats = mock_agent_service_->getUrlStats();
    const auto config = mock_agent_service_->getConfig();

    // Three consecutive ticks: each entry's arrival cuts the previous tick
    // into completed_, leaving the last one open. Nothing consumes them, as
    // if the stats stream had been down the whole time.
    for (const int64_t second : {1000, 1030, 1060}) {
        UrlStatEntry entry{"/api/retained", "GET", 200};
        entry.elapsed_ = 10;
        entry.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(second));
        url_stats.addSnapshot(&entry, *config);
    }

    mock_agent_service_->setExiting(true);
    stats_client.setStatsChannelReady(true);

    ASSERT_EQ(stats_client.buildShutdownFlushForTest(), STREAM_WRITE);
    const auto* msg = stats_client.pendingMessageForTest();
    ASSERT_NE(msg, nullptr);
    ASSERT_TRUE(msg->has_agenturistat());
    const auto& uri_stat = msg->agenturistat();
    ASSERT_EQ(uri_stat.eachuristat_size(), 3)
        << "the two retained completed ticks must leave with the open one";
    std::vector<int64_t> ticks;
    for (const auto& each : uri_stat.eachuristat()) {
        EXPECT_EQ(each.uri(), "/api/retained");
        ticks.push_back(each.timestamp());
    }
    std::sort(ticks.begin(), ticks.end());
    EXPECT_EQ(ticks, (std::vector<int64_t>{990000, 1020000, 1050000}));

    // The flush is a drain: a second one has nothing left to send, so a
    // worker that reaches both flush call sites cannot double-send.
    EXPECT_EQ(stats_client.buildShutdownFlushForTest(), STREAM_CONTINUE);
    EXPECT_EQ(stats_client.shutdownDroppedUrlStatsForTest(), 0U)
        << "an empty drain is not a drop";
}

// A channel that is not READY cannot deliver a write inside the shutdown
// deadline, so the flush must not start one — it records the loss instead.
// Same policy, and the same reason, as GrpcSpan::flush_remaining's
// channel-state probe.
TEST_F(GrpcMockTest, GrpcStatsShutdownFlushDropsAndCountsWhenChannelNotReady) {
    TestableGrpcStats stats_client(mock_agent_service_.get());
    auto& url_stats = mock_agent_service_->getUrlStats();
    const auto config = mock_agent_service_->getConfig();

    for (const auto* uri : {"/api/dropped/one", "/api/dropped/two"}) {
        UrlStatEntry entry{uri, "GET", 200};
        entry.elapsed_ = 10;
        entry.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(1000));
        url_stats.addSnapshot(&entry, *config);
    }

    mock_agent_service_->setExiting(true);
    stats_client.setStatsChannelReady(false);

    EXPECT_EQ(stats_client.buildShutdownFlushForTest(), STREAM_CONTINUE)
        << "no write may be attempted on a channel that is not READY";
    EXPECT_EQ(stats_client.pendingMessageForTest(), nullptr)
        << "nothing may be built for a send that cannot happen";
    EXPECT_EQ(stats_client.shutdownDroppedUrlStatsForTest(), 2U);

    // Nothing is left to drop, so the count stops moving.
    EXPECT_EQ(stats_client.buildShutdownFlushForTest(), STREAM_CONTINUE);
    EXPECT_EQ(stats_client.shutdownDroppedUrlStatsForTest(), 2U);

    // The worker-exit call site: with no stats stream ever started there is
    // nothing to write on, whatever the channel says, and the same count is
    // the only record the entries leave.
    stats_client.setStatsChannelReady(true);
    UrlStatEntry late{"/api/dropped/three", "GET", 200};
    late.elapsed_ = 10;
    late.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(1000));
    url_stats.addSnapshot(&late, *config);

    stats_client.flushUrlStatsOnShutdownForTest();
    EXPECT_EQ(stats_client.shutdownDroppedUrlStatsForTest(), 3U)
        << "a flush with no live stream must count the drop, not send";
}

// Reachability, driven through the real worker rather than the seam: the
// original bug was not a wrong flush but an unreachable one, so the call site
// is what needs locking. With readyChannel() false the worker never opens a
// stream and ends on the stop request, and sendStatsWorker's exit-path flush
// is the only thing that can account for the entries.
TEST_F(GrpcMockTest, GrpcStatsWorkerRunsShutdownFlushOnEveryExitPath) {
    GrpcClientTuning tuning;
    tuning.worker_restart_delay = std::chrono::milliseconds(10);
    TestableGrpcStats stats_client(mock_agent_service_.get(), tuning);
    stats_client.setMockStatsStub(std::make_unique<NiceMock<v1::MockStatStub>>());
    stats_client.setReadyChannel(false);

    UrlStatEntry entry{"/api/worker-exit", "GET", 200};
    entry.elapsed_ = 10;
    entry.end_time_ = std::chrono::system_clock::time_point(std::chrono::seconds(1000));
    mock_agent_service_->getUrlStats().addSnapshot(&entry, *mock_agent_service_->getConfig());

    ScopedWorker stats_worker([&stats_client] { stats_client.stopStatsWorker(); },
                              [&stats_client] { stats_client.sendStatsWorker(); });

    mock_agent_service_->setExiting(true);
    stats_client.stopStatsWorker();
    if (stats_worker.joinable()) stats_worker.join();

    EXPECT_EQ(stats_client.shutdownDroppedUrlStatsForTest(), 1U)
        << "the worker must reach the shutdown flush even with no stream to write on";
    // And the snapshot really was drained by the worker, not merely observed.
    EXPECT_TRUE(mock_agent_service_->getUrlStats().takeSnapshot(true)->empty());
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

    agent.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "dead.channel")));
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

    agent.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "api.ok")));
    agent.enqueueMeta(std::make_unique<MetaData>(ApiMeta(2, 100, "api.fail")));
    agent.enqueueMeta(std::make_unique<MetaData>(ApiMeta(3, 100, "api.recover")));

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

// PResult.success=false is the collector rejecting the request's content, not
// a delivery failure: resending the same bytes earns the same answer, so the
// item is dropped at once and its cache entry released, letting a later span
// re-register the id and send a genuinely new request.
TEST_F(GrpcMockTest, GrpcMetadataDropsRejectedResultAndEvictsCache) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(std::chrono::milliseconds(50));

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    fake->setReplyMode(FakeMetadataStub::ReplyMode::RESULT_FAIL);

    metadata.setMockMetaStub(std::move(fake_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "api.rejected")));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    EXPECT_TRUE(wait_for_condition(
        [this] { return mock_agent_service_->removed_api_count_.load() >= 1; },
        std::chrono::seconds(5)));
    // Well past the 50ms retry delay: a scheduled retry would have fired.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::API), 1u);
    EXPECT_EQ(mock_agent_service_->removed_api_count_.load(), 1);
}

TEST_F(GrpcMockTest, GrpcMetadataDelaysCacheReleaseAfterPermanentRejection) {
    constexpr auto kDelay = std::chrono::milliseconds(800);
    constexpr auto kWellInsideDelay = std::chrono::milliseconds(200);

    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(kDelay);

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    fake->setReplyMode(FakeMetadataStub::ReplyMode::ERROR_STATUS);
    fake->setErrorStatus(grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "no such method"));

    metadata.setMockMetaStub(std::move(fake_meta_stub));
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "api.rejected")));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    // The item is sent once and refused.
    ASSERT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::API, 1,
                                          std::chrono::seconds(5)));

    // An inline release would already have landed by now; the parked one
    // cannot, so nothing has re-opened the id for the next span yet.
    std::this_thread::sleep_for(kWellInsideDelay);
    EXPECT_EQ(mock_agent_service_->removed_api_count_.load(), 0)
        << "the cache release must wait out the retry delay, not fire inline";

    // Once the delay expires the entry is released, so a later span can
    // re-register the id and probe the collector again.
    EXPECT_TRUE(wait_for_condition(
        [this] { return mock_agent_service_->removed_api_count_.load() == 1; },
        std::chrono::seconds(5)))
        << "the parked release must still happen, or the id never recovers";

    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::API), 1u);
}

TEST_F(GrpcMockTest, GrpcMetadataRetriesOnlyTransientStatusCodes) {
    struct Case {
        grpc::StatusCode code;
        size_t expected_attempts;  // initial send + scheduled retries
    };
    const Case cases[] = {
        {grpc::StatusCode::UNAVAILABLE, 4},
        {grpc::StatusCode::DEADLINE_EXCEEDED, 4},
        {grpc::StatusCode::INVALID_ARGUMENT, 1},
        {grpc::StatusCode::UNIMPLEMENTED, 1},
        {grpc::StatusCode::PERMISSION_DENIED, 1},
        {grpc::StatusCode::UNAUTHENTICATED, 1},
        {grpc::StatusCode::NOT_FOUND, 1},
        {grpc::StatusCode::RESOURCE_EXHAUSTED, 1},
        {grpc::StatusCode::ABORTED, 1},
        {grpc::StatusCode::INTERNAL, 1},
        {grpc::StatusCode::UNKNOWN, 1},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE("status code " + std::to_string(static_cast<int>(c.code)));
        const auto released_before = mock_agent_service_->removed_api_count_.load();

        TestableGrpcMetadata metadata(mock_agent_service_.get());
        metadata.setRetryDelay(std::chrono::milliseconds(50));

        auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
        auto* fake = fake_meta_stub.get();
        fake->setReplyMode(FakeMetadataStub::ReplyMode::ERROR_STATUS);
        fake->setErrorStatus(grpc::Status(c.code, "scripted failure"));

        metadata.setMockMetaStub(std::move(fake_meta_stub));
        metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "api.status")));

        ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                         [&metadata] { metadata.sendMetaWorker(); });

        // Both outcomes end in exactly one cache release, one retry delay
        // later: parked for a permanent status, after retry exhaustion for a
        // transient one.
        EXPECT_TRUE(wait_for_condition(
            [this, released_before] {
                return mock_agent_service_->removed_api_count_.load() > released_before;
            },
            std::chrono::seconds(10)));
        // Long enough for another retry to fire if one were scheduled.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        metadata.stopMetaWorker();
        if (meta_worker.joinable()) meta_worker.join();

        EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::API), c.expected_attempts);
        EXPECT_EQ(mock_agent_service_->removed_api_count_.load() - released_before, 1);
    }
}

// A permanent status must not tie up the pipeline: the item is attempted once
// and never resent, and every permit comes back, so far more items than
// meta_max_concurrent_requests still drain and the next healthy send goes out
// immediately. Each one does take a retry-schedule slot on its way out — that
// is the parked cache release — but only until its delay expires.
TEST_F(GrpcMockTest, GrpcMetadataNonRetryableFailuresFreePermitsAndRetryQueue) {
    constexpr int kItems = 20;  // >> meta_max_concurrent_requests (4)

    TestableGrpcMetadata metadata(mock_agent_service_.get());
    metadata.setRetryDelay(std::chrono::milliseconds(50));

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    fake->setReplyMode(FakeMetadataStub::ReplyMode::ERROR_STATUS);
    fake->setErrorStatus(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "schema mismatch"));

    metadata.setMockMetaStub(std::move(fake_meta_stub));
    for (int i = 0; i < kItems; ++i) {
        metadata.enqueueMeta(std::make_unique<MetaData>(
            ApiMeta(i + 1, 100, "api.permanent." + std::to_string(i))));
    }

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    EXPECT_TRUE(wait_for_condition(
        [this] { return mock_agent_service_->removed_api_count_.load() >= kItems; },
        std::chrono::seconds(10)));

    // The pipeline is not stalled: with the fault cleared, the very next item
    // is sent — impossible if the drops had kept their permits.
    fake->setReplyMode(FakeMetadataStub::ReplyMode::OK);
    metadata.enqueueMeta(std::make_unique<MetaData>(
        StringMeta(1, "error.after.drops", STRING_META_ERROR)));
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::STRING, 1,
                                          std::chrono::seconds(5)));

    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    // One attempt each: no item was ever resent.
    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::API), static_cast<size_t>(kItems));
    EXPECT_EQ(mock_agent_service_->removed_api_count_.load(), kItems);
    EXPECT_EQ(mock_agent_service_->removed_error_count_.load(), 0);
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
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "api.throw.retry")));

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
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "api.not.ready")));

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
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "api.exhaust")));

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
        StringMeta(2, "error.exhaust", STRING_META_ERROR)));

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
        StringMeta(3, "SELECT exhaust", STRING_META_SQL)));

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
        SqlUidMeta(uid, "SELECT uid_exhaust")));

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

TEST_F(GrpcMockTest, GrpcMetadataAbbreviatesSqlOverMetadataCapOnTheWire) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    metadata.setMockMetaStub(std::move(fake_meta_stub));

    const std::string long_sql(70000, 'a');
    const std::string expected =
        std::string(kMaxSqlMetaLength, 'a') + "...(70000)";
    const SqlUid uid{0, 1, 2, 3, 4, 5, 6, 7,
                     8, 9, 10, 11, 12, 13, 14, 15};
    metadata.enqueueMeta(std::make_unique<MetaData>(
        StringMeta(7, long_sql, STRING_META_SQL)));
    metadata.enqueueMeta(std::make_unique<MetaData>(
        SqlUidMeta(uid, long_sql)));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::SQL, 1,
                                          std::chrono::seconds(5)));
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::SQL_UID, 1,
                                          std::chrono::seconds(5)));

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(fake->sqlRequest(0).sql(), expected);
    EXPECT_EQ(fake->sqlUidRequest(0).sql(), expected);
    EXPECT_EQ(fake->sqlRequest(0).sqlid(), 7);
    // Both were accepted, so neither cache entry was released.
    EXPECT_EQ(mock_agent_service_->removed_sql_count_.load(), 0);
    EXPECT_EQ(mock_agent_service_->removed_sql_uid_count_.load(), 0);
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
    agent.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "test.api")));
    agent.enqueueMeta(std::make_unique<MetaData>(StringMeta(2, "error msg", STRING_META_ERROR)));
    agent.enqueueMeta(std::make_unique<MetaData>(StringMeta(3, "SELECT 1", STRING_META_SQL)));

    SqlUid uid = {1, 2, 3};
    agent.enqueueMeta(std::make_unique<MetaData>(SqlUidMeta(uid, "SELECT * FROM t")));

    TraceId txid{"agent", 100, 0};
    std::vector<std::unique_ptr<Exception>> exceptions;
    auto cs = std::make_unique<CallStack>("err");
    exceptions.push_back(std::make_unique<Exception>(std::move(cs)));
    agent.enqueueMeta(std::make_unique<MetaData>(ExceptionMeta(txid, 1, "/api", std::move(exceptions))));

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

    agent.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "test.api")));
    agent.enqueueMeta(std::make_unique<MetaData>(StringMeta(2, "err", STRING_META_ERROR)));
    agent.enqueueMeta(std::make_unique<MetaData>(StringMeta(3, "SELECT 1", STRING_META_SQL)));

    SqlUid uid = {1, 2, 3};
    agent.enqueueMeta(std::make_unique<MetaData>(SqlUidMeta(uid, "SELECT * FROM t")));

    TraceId txid{"agent", 100, 0};
    std::vector<std::unique_ptr<Exception>> exceptions;
    auto cs = std::make_unique<CallStack>("err");
    exceptions.push_back(std::make_unique<Exception>(std::move(cs)));
    agent.enqueueMeta(std::make_unique<MetaData>(ExceptionMeta(txid, 1, "/api", std::move(exceptions))));

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

    agent.enqueueMeta(std::make_unique<MetaData>(StringMeta(1, "SELECT * FROM users", STRING_META_SQL)));

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
    agent.enqueueMeta(std::make_unique<MetaData>(SqlUidMeta(uid, "INSERT INTO t VALUES (?)")));

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

    agent.enqueueMeta(std::make_unique<MetaData>(ExceptionMeta(txid, 999, "/api/v2/resource", std::move(exceptions))));

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
        .WillOnce(DoAll(SetArgPointee<2>(accepted_result()), Return(grpc::Status::OK)))
        .WillOnce(Return(grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "already registered")))
        .WillOnce(DoAll(SetArgPointee<2>(accepted_result()), Return(grpc::Status::OK)));

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
    span_data->getAnnotations()->AppendData(105, AnnotationData(7, std::make_shared<const std::string>("method"), "GET"));
    span_data->getAnnotations()->AppendLongIntIntByteByteString(106, 99, 1, 2, 3, 4, "rpc");
    span_data->getAnnotations()->AppendData(107, AnnotationData(uid, std::make_shared<const std::string>("sql"), "args"));
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

TEST_F(GrpcMockTest, GrpcSpanBuilderReplacesInvalidUtf8InStringFields) {
    const std::string bad = "\xff\xfe";
    const std::string fffd = "\xef\xbf\xbd";
    auto span_parent = std::make_shared<SpanImpl>(mock_agent_service_.get(), "utf8-op", "/rpc/" + bad);
    auto span_data = span_parent->getSpanData();
    span_data->setTraceId(mock_agent_service_->generateTraceId());
    span_data->setEndPoint("host" + bad);
    span_data->setRemoteAddr("caf\xe9");
    span_data->getAnnotations()->AppendData(103, AnnotationData("ann" + bad));
    span_data->getAnnotations()->AppendStringString(104, "l" + bad, "r" + bad);
    auto span_event = make_test_span_event_unique(*span_parent, "child-op");
    span_event->SetDestination("redis" + bad);
    span_event->SetEndPoint("ep" + bad);
    span_event->SetError("Err" + bad, "msg" + bad);
    span_event->SetAnnotation(201, "ev" + bad);
    span_data->addSpanEvent(std::move(span_event));
    span_data->finishSpanEvent(span_data->topSpanEvent());

    google::protobuf::Arena arena;
    const auto* span = build_grpc_span(std::make_unique<SpanChunk>(span_data, true), &arena);
    ASSERT_NE(span, nullptr);

    const auto valid = [&](const std::string& field) { return toValidUtf8(field) == field; };
    EXPECT_EQ(span->acceptevent().rpc(), "/rpc/" + fffd);
    EXPECT_EQ(span->acceptevent().endpoint(), "host" + fffd);
    EXPECT_EQ(span->acceptevent().remoteaddr(), "caf" + fffd);
    ASSERT_EQ(span->annotation_size(), 2);
    EXPECT_EQ(span->annotation(0).value().stringvalue(), "ann" + fffd);
    EXPECT_TRUE(valid(span->annotation(1).value().stringstringvalue().stringvalue1().value()));
    EXPECT_TRUE(valid(span->annotation(1).value().stringstringvalue().stringvalue2().value()));
    ASSERT_EQ(span->spanevent_size(), 1);
    const auto& event = span->spanevent(0);
    EXPECT_EQ(event.nextevent().messageevent().destinationid(), "redis" + fffd);
    EXPECT_EQ(event.nextevent().messageevent().endpoint(), "ep" + fffd);
    EXPECT_TRUE(valid(event.exceptioninfo().stringvalue().value()));
    ASSERT_EQ(event.annotation_size(), 1);
    EXPECT_EQ(event.annotation(0).value().stringvalue(), "ev" + fffd);
}

TEST_F(GrpcMockTest, GrpcMetadataReplacesInvalidUtf8OnTheWire) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());
    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    metadata.setMockMetaStub(std::move(fake_meta_stub));

    const std::string bad = "\xff";
    const std::string fffd = "\xef\xbf\xbd";
    // Longer than the cap so the truncate-then-replace order is exercised:
    // the sanitized text must stay within the abbreviated shape.
    const std::string long_sql = std::string(kMaxSqlMetaLength - 1, 'a') + bad + std::string(10, 'b');
    const SqlUid uid{1, 2, 3};
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "api" + bad)));
    metadata.enqueueMeta(std::make_unique<MetaData>(StringMeta(2, "err" + bad, STRING_META_ERROR)));
    metadata.enqueueMeta(std::make_unique<MetaData>(StringMeta(3, long_sql, STRING_META_SQL)));
    metadata.enqueueMeta(std::make_unique<MetaData>(SqlUidMeta(uid, "SELECT " + bad)));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::API, 1, std::chrono::seconds(5)));
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::STRING, 1, std::chrono::seconds(5)));
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::SQL, 1, std::chrono::seconds(5)));
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::SQL_UID, 1, std::chrono::seconds(5)));
    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(fake->apiRequest(0).apiinfo(), "api" + fffd);
    EXPECT_EQ(fake->stringRequest(0).stringvalue(), "err" + fffd);
    EXPECT_EQ(fake->sqlRequest(0).sql(),
              std::string(kMaxSqlMetaLength - 1, 'a') + fffd + "...(" + std::to_string(long_sql.size()) + ")");
    EXPECT_EQ(fake->sqlUidRequest(0).sql(), "SELECT " + fffd);
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
    EXPECT_EQ(span_client.droppedSpans(), 1u)
        << "a batch dropped for want of a permit must be counted as lost (T-3)";

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

    // T-8: spans lost to a failed RPC are counted, not just logged. Both
    // batches failed (the fake answers every call with an error status).
    EXPECT_EQ(span_client.droppedSpans(), 2u)
        << "every span whose RPC failed must be counted as lost";
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

// Production outage behavior: readyChannel() blocks the worker on the batch
// it already collected, the queue keeps filling behind it (oldest-drop once
// full), and on recovery the held batch and the queued spans are delivered.
// Nothing is dropped for the outage itself.
TEST_F(GrpcMockTest, GrpcSpanCollectorOutageQueuesSpansAndRecoveryDeliversThem) {
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

    auto outage_span = make_test_span_data_ptr(*mock_agent_service_, "outage-op");
    const auto outage_api_id = outage_span->getApiId();
    span_client.enqueueSpan(std::make_unique<SpanChunk>(outage_span, true));
    auto queued_span = make_test_span_data_ptr(*mock_agent_service_, "queued-op");
    const auto queued_api_id = queued_span->getApiId();
    span_client.enqueueSpan(std::make_unique<SpanChunk>(queued_span, true));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(fake->batchCount(), 0u) << "nothing can be sent while the channel is down";

    span_client.setReadyChannel(true);
    ASSERT_TRUE(fake->waitForBatchCount(2, std::chrono::seconds(2)))
        << "the batch held through the outage and the queued span must both be sent on recovery";

    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();

    ASSERT_EQ(fake->batchCount(), 2u);
    ASSERT_EQ(fake->request(0).span_size(), 1);
    ASSERT_EQ(fake->request(1).span_size(), 1);
    EXPECT_EQ(fake->request(0).span(0).span().apiid(), outage_api_id)
        << "the batch collected during the outage is sent first, not dropped";
    EXPECT_EQ(fake->request(1).span(0).span().apiid(), queued_api_id);
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
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "api.outage.recovery")));

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

    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "overflow.api.1")));
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(2, 100, "overflow.api.2")));
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(3, 100, "overflow.api.3")));

    // The dropped item's cache entry is released synchronously on the
    // producer thread, before any worker runs: otherwise the id stays
    // marked published and is never re-sent.
    EXPECT_EQ(mock_agent_service_->removed_api_count_, 1)
        << "the overflow-dropped metadata must release its cache entry";

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    // Only the 2 queued metas may be sent; the third was dropped on enqueue.
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::API, 2, std::chrono::seconds(5)));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();

    if (meta_worker.joinable()) meta_worker.join();

    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::API), 2u);
    EXPECT_EQ(mock_agent_service_->removed_api_count_, 1)
        << "the two delivered items must keep their cache entries";
}

TEST_F(GrpcMockTest, GrpcMetadataRetryBacklogDoesNotStarveNewMetaQueue) {
    mock_agent_service_->mutableConfig()->collector.grpc.channel.sender_queue_size = 2;

    GrpcClientTuning tuning;
    tuning.meta_retry_queue_size = 1;
    tuning.meta_retry_delay = std::chrono::seconds(60);
    TestableGrpcMetadata metadata(mock_agent_service_.get(), tuning);

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    metadata.setMockMetaStub(std::move(fake_meta_stub));
    // The collector is down for the rest of the test: every dequeued item
    // fails its readiness check and enters the retry schedule.
    metadata.setReadyChannel(false);

    const SqlUid uid{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "retry.head")));
    metadata.enqueueMeta(std::make_unique<MetaData>(SqlUidMeta(uid, "SELECT retry_tail")));

    {
        ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                                 [&metadata] { metadata.sendMetaWorker(); });

        // Both items reach the retry schedule, whose cap is 1: the second
        // head-drops the first, releasing the ApiMeta's cache entry.
        EXPECT_TRUE(wait_for_condition(
            [this] { return mock_agent_service_->removed_api_count_.load() >= 1; },
            std::chrono::seconds(5)))
            << "the head-dropped retry must release its cache entry";
        // Give a second (wrong) release the chance to land before the counts
        // below are read.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        metadata.stopMetaWorker();
        if (meta_worker.joinable()) meta_worker.join();
    }

    // Exactly one release for the head-dropped item, and none for the item
    // still parked in the retry schedule.
    EXPECT_EQ(mock_agent_service_->removed_api_count_, 1)
        << "the head-dropped retry must release its cache entry exactly once";
    EXPECT_EQ(mock_agent_service_->removed_sql_uid_count_, 0)
        << "the retained retry must keep its cache entry";

    // The worker is stopped, so the retry schedule stays full (1/1) and the
    // new queue stays empty for the rest of the test.
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(2, 100, "new.after.backlog.1")));
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(3, 100, "new.after.backlog.2")));

    // A full retry schedule must not shrink the new queue: both items fit its
    // own budget of 2. Under the shared budget the second was dropped here,
    // its cache entry released, and the same metadata re-enqueued by the next
    // span that used it.
    EXPECT_EQ(mock_agent_service_->removed_api_count_, 1)
        << "new metadata must be accepted while the retry schedule is full";

    // The two bounds still cap the pipeline: the new queue is now full too,
    // so the next item is dropped, holding the total at 2 + 1.
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(4, 100, "new.after.backlog.3")));
    EXPECT_EQ(mock_agent_service_->removed_api_count_, 2)
        << "the new queue must still enforce its own bound";
    EXPECT_EQ(mock_agent_service_->removed_sql_uid_count_, 0);
    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::API), 0u)
        << "no RPC can have been sent while the channel was never ready";
}

TEST_F(GrpcMockTest, GrpcMetadataWorkerKeepsDrainingQueueDuringCollectorOutage) {
    mock_agent_service_->mutableConfig()->collector.grpc.channel.sender_queue_size = 1;

    GrpcClientTuning tuning;
    tuning.meta_retry_delay = std::chrono::milliseconds(200);
    tuning.meta_retry_max_attempts = 100;
    TestableGrpcMetadata metadata(mock_agent_service_.get(), tuning);
    metadata.setReadyChannel(false);

    auto fake_meta_stub = std::make_unique<FakeMetadataStub>();
    auto* fake = fake_meta_stub.get();
    metadata.setMockMetaStub(std::move(fake_meta_stub));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                             [&metadata] { metadata.sendMetaWorker(); });

    for (int i = 1; i <= 5; ++i) {
        metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(i, 100, "outage.api." + std::to_string(i))));
        // Let the worker take each item before the next arrives: a worker
        // stuck in a readiness wait cannot, and the queue (size 1) overflows.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(fake->requestCount(FakeMetadataStub::MetaRpc::API), 0u)
        << "no RPC can be sent while the channel is down";
    EXPECT_EQ(mock_agent_service_->removed_api_count_, 0)
        << "a draining worker never overflows the new queue, so no cache entry is released";

    metadata.setReadyChannel(true);
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::API, 5, std::chrono::seconds(5)))
        << "every item parked through the outage must be delivered on recovery";

    mock_agent_service_->setExiting(true);
    metadata.stopMetaWorker();
    if (meta_worker.joinable()) meta_worker.join();
    EXPECT_EQ(mock_agent_service_->removed_api_count_, 0);
}

TEST_F(GrpcMockTest, GrpcMetadataEnqueueNullMetaIsNoop) {
    TestableGrpcMetadata metadata(mock_agent_service_.get());

    auto mock_meta_stub = std::make_unique<StrictMock<v1::MockMetadataStub>>();
    metadata.setMockMetaStub(std::move(mock_meta_stub));

    metadata.enqueueMeta(nullptr);

    SUCCEED() << "Null metadata must be ignored without touching the queue or stub";
}


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
                                            v1::PResult* reply) {
            captured_budget = std::chrono::duration_cast<std::chrono::milliseconds>(
                ctx->deadline() - std::chrono::system_clock::now());
            reply->set_success(true);
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
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "api.injected.retry")));

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
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "pipeline.1")));
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(2, 100, "pipeline.2")));
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(3, 100, "pipeline.3")));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

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

    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "api.ready.throw")));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    // The item survives the exception on the retry path and is delivered.
    EXPECT_TRUE(fake->waitForRequestCount(FakeMetadataStub::MetaRpc::API, 1, std::chrono::seconds(5)))
        << "the item dequeued before the throwing readiness check must not be lost";

    // The permit survives too: a second item can still launch.
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(2, 100, "api.after.throw")));
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

    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "api.alloc.fail")));
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(2, 100, "api.alloc.after")));

    ScopedWorker meta_worker([&metadata] { metadata.stopMetaWorker(); },
                     [&metadata] { metadata.sendMetaWorker(); });

    ASSERT_TRUE(fake->waitForHeldCallbacks(1, std::chrono::seconds(5)))
        << "the first item must be in flight holding the single permit";

    // A failed status whose message outgrows any SSO buffer: completing with
    // it makes every string copy on the completion path allocate. Built
    // before arming, so its own allocations run unrestricted.
    grpc::Status failed_status(grpc::StatusCode::UNAVAILABLE, std::string(192, 'x'));

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
    metadata.enqueueMeta(std::make_unique<MetaData>(ApiMeta(1, 100, "shutdown.hold")));

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

// ============================================================
// Channel rotation while a SendSpanBatch is still in flight on the previous
// transport: the pending call's snapshot must keep that transport alive until
// its completion callback has run (stub access invariant, grpc.h). Real
// channels to a bare in-process server drive the real readyChannel() path;
// the fake stubs only stand in for the RPC itself.
// ============================================================

namespace {

// Publishes a fresh FakeSpanStub on every channel (the initial open and each
// rotation) so the test can see which transport a batch was launched on.
class RotatingFakeStubGrpcSpan : public GrpcSpan {
public:
    RotatingFakeStubGrpcSpan(AgentService* agent, const GrpcClientTuning& tuning)
        : GrpcSpan(agent->getConfig(), tuning) {
        agent_ = agent;
    }

    std::weak_ptr<const Transport> transportRef() const { return current_transport<SpanStub>(); }

    uint32_t generation() const {
        const auto transport = current_transport<SpanStub>();
        return transport ? transport->generation : 0;
    }

    // Raw: each stub is owned by its transport and dies with it.
    FakeSpanStub* stub(size_t index) {
        std::lock_guard<std::mutex> lock(mutex_);
        return index < stubs_.size() ? stubs_[index] : nullptr;
    }

    bool waitForStubCount(size_t count, std::chrono::milliseconds timeout) {
        return wait_for_condition([&] {
            std::lock_guard<std::mutex> lock(mutex_);
            return stubs_.size() >= count;
        }, timeout);
    }

protected:
    void create_stub(const std::shared_ptr<grpc::Channel>& channel) override {
        auto stub = std::make_unique<FakeSpanStub>();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stubs_.push_back(stub.get());
        }
        set_span_stub(std::move(stub), channel);
    }

private:
    std::mutex mutex_;
    std::vector<FakeSpanStub*> stubs_;
};

}  // namespace

TEST_F(GrpcMockTest, GrpcSpanRotationKeepsPreviousTransportAliveWhileBatchInFlight) {
    BareGrpcServer server;
    ASSERT_NE(server.server, nullptr);
    auto& cfg = mock_agent_service_->mutableConfig();
    cfg->collector.host = "127.0.0.1";
    cfg->collector.span_port = server.port;
    cfg->collector.span_batch.size = 1;
    cfg->collector.span_batch.flush_interval_ms = 50;
    cfg->collector.span_batch.collect_deadline_ms = 10;
    cfg->collector.grpc.channel.channel_max_age_ms = 200;

    GrpcClientTuning tuning;
    tuning.channel_rotation_ready_timeout = std::chrono::seconds(5);
    RotatingFakeStubGrpcSpan span_client(mock_agent_service_.get(), tuning);
    span_client.openChannel();
    ASSERT_EQ(span_client.generation(), 1u);
    auto* first_stub = span_client.stub(0);
    ASSERT_NE(first_stub, nullptr);
    first_stub->setReplyMode(FakeSpanStub::ReplyMode::HOLD);

    ScopedWorker worker([&span_client] { span_client.stopSpanWorker(); },
                        [&span_client] { span_client.sendSpanWorker(); });

    // A batch launched on transport #1 whose completion is withheld.
    auto held_span = make_test_span_data_ptr(*mock_agent_service_, "held-op");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(held_span, true));
    ASSERT_TRUE(first_stub->waitForBatchCount(1, std::chrono::seconds(5)));
    const auto previous = span_client.transportRef();
    ASSERT_FALSE(previous.expired());

    // Age the channel past 200ms +-10%, then send again: the worker's
    // readyChannel() rotates to transport #2 and the new batch goes there.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto later_span = make_test_span_data_ptr(*mock_agent_service_, "later-op");
    span_client.enqueueSpan(std::make_unique<SpanChunk>(later_span, true));
    ASSERT_TRUE(span_client.waitForStubCount(2, std::chrono::seconds(5)));
    auto* second_stub = span_client.stub(1);
    ASSERT_NE(second_stub, nullptr);
    ASSERT_TRUE(second_stub->waitForBatchCount(1, std::chrono::seconds(5)));
    EXPECT_EQ(span_client.generation(), 2u);

    EXPECT_FALSE(previous.expired())
        << "a transport with a call in flight must outlive its replacement";

    // Completing the held call releases the last snapshot.
    first_stub->releaseHeldCallbacks(grpc::Status::OK);
    first_stub = nullptr;  // owned by transport #1, which is gone from here
    EXPECT_TRUE(wait_for_condition([&previous] { return previous.expired(); },
                                   std::chrono::seconds(2)))
        << "once the last call on it completes, the previous transport is destroyed";
    EXPECT_EQ(second_stub->batchCount(), 1u);

    mock_agent_service_->setExiting(true);
    span_client.stopSpanWorker();
    if (worker.joinable()) worker.join();
}


// ============================================================
// Unbounded OnDone waits stay observable
// ============================================================

// A callback stream whose OnDone only the test delivers: the mock equivalent
// of a collector that never completes the call, not even after TryCancel.
// Binds itself to the reactor so StartCall/StartRead/StartWritesDone/holds
// land here instead of in a null gRPC stream.
class HeldPingStream final : public grpc::ClientCallbackReaderWriter<v1::PPing, v1::PPing> {
public:
    explicit HeldPingStream(grpc::ClientBidiReactor<v1::PPing, v1::PPing>* reactor)
        : reactor_(reactor) { BindReactor(reactor); }

    void StartCall() override {}
    void Write(const v1::PPing*, grpc::WriteOptions) override { write_pending_ = true; }
    void WritesDone() override { writes_done_ = true; }
    void Read(v1::PPing*) override {}
    void AddHold(int) override {}
    void RemoveHold() override {}

    bool writePending() const { return write_pending_; }
    bool writesDone() const { return writes_done_; }
    // Ack the ping and deliver a pong, off the worker thread like gRPC does.
    void pong() { reactor_->OnWriteDone(true); reactor_->OnReadDone(true); }
    void deliverOnDone() { reactor_->OnDone(grpc::Status::CANCELLED); }

private:
    grpc::ClientBidiReactor<v1::PPing, v1::PPing>* reactor_;
    std::atomic<bool> write_pending_{false};
    std::atomic<bool> writes_done_{false};
};

class HeldPingAgentStub final : public NiceMock<v1::MockAgentStub> {
public:
    HeldPingAgentStub() : async_(this) {}
    async_interface* async() override { return &async_; }
    HeldPingStream* stream() const { return stream_.load(); }

private:
    class Async final : public async_interface {
    public:
        explicit Async(HeldPingAgentStub* owner) : owner_(owner) {}
        void RequestAgentInfo(grpc::ClientContext*, const v1::PAgentInfo*, v1::PResult*,
                              std::function<void(grpc::Status)>) override {}
        void RequestAgentInfo(grpc::ClientContext*, const v1::PAgentInfo*, v1::PResult*,
                              grpc::ClientUnaryReactor*) override {}
        void PingSession(grpc::ClientContext*,
                         grpc::ClientBidiReactor<v1::PPing, v1::PPing>* reactor) override {
            owner_->stream_.store(owner_->streams_.emplace_back(
                std::make_unique<HeldPingStream>(reactor)).get());
        }
    private:
        HeldPingAgentStub* owner_;
    };

    Async async_;
    std::vector<std::unique_ptr<HeldPingStream>> streams_;  // worker thread only
    std::atomic<HeldPingStream*> stream_{nullptr};
};

// Routes the agent log into a file for the test's lifetime.
class LogCapture {
public:
    LogCapture() : path_(std::filesystem::temp_directory_path() /
                         "pinpoint_grpc_with_mocks_log.txt") {
        Logger::getInstance().shutdown();
        std::filesystem::remove(path_);
        Logger::getInstance().setLogLevel("info");
        Logger::getInstance().setFileLogger(path_.string(), 10);
    }
    ~LogCapture() {
        Logger::getInstance().shutdown();
        Logger::getInstance().setFileLogger("", 0);
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    std::string text() {
        Logger::getInstance().shutdown();
        std::ifstream ifs(path_);
        return {std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
    }
    static size_t count(const std::string& text, std::string_view needle) {
        size_t n = 0;
        for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1)) ++n;
        return n;
    }
private:
    std::filesystem::path path_;
};

// Runs a ping stream up to the finish path with OnDone withheld and stores
// the captured log once the worker has exited. `hold_for` is how long OnDone
// is withheld after the finish path asked for WritesDone.
static void run_ping_finish_with_held_on_done(MockAgentService& service,
                                              const GrpcClientTuning& tuning,
                                              std::chrono::milliseconds hold_for,
                                              std::string* log_out) {
    TestableGrpcAgent agent(&service, tuning);
    auto stub = std::make_unique<HeldPingAgentStub>();
    auto* held = stub.get();
    agent.setMockAgentStub(std::move(stub));

    LogCapture log;
    ScopedWorker ping_worker([&agent] { agent.stopPingWorker(); },
                             [&agent] { agent.sendPingWorker(); });
    // First ping written: answer it so the worker parks in its interval wait.
    ASSERT_TRUE(wait_for_condition(
        [held] { return held->stream() != nullptr && held->stream()->writePending(); },
        std::chrono::seconds(3)));
    held->stream()->pong();

    service.setExiting(true);
    agent.stopPingWorker();
    ASSERT_TRUE(wait_for_condition([held] { return held->stream()->writesDone(); },
                                   std::chrono::seconds(3)));
    std::this_thread::sleep_for(hold_for);
    held->stream()->deliverOnDone();
    ping_worker.join();
    *log_out = log.text();
}

TEST_F(GrpcMockTest, GrpcAgentWarnsPeriodicallyWhileOnDoneIsWithheld) {
    GrpcClientTuning tuning;
    tuning.stream_finish_timeout = std::chrono::milliseconds(20);
    tuning.stream_wait_warn_interval = std::chrono::milliseconds(50);

    // 20ms bounded wait, then ~6 warn intervals of withheld OnDone.
    std::string log;
    run_ping_finish_with_held_on_done(*mock_agent_service_, tuning, std::chrono::milliseconds(350), &log);
    // Timing slack: at least half the intervals must have been reported, and
    // the wait must still have ended normally (the worker joined above).
    EXPECT_GE(LogCapture::count(log, "agent grpc stream ping finish: still waiting for OnDone"), 3u)
        << log;
}

TEST_F(GrpcMockTest, GrpcAgentPromptOnDoneLogsNoWarning) {
    GrpcClientTuning tuning;
    tuning.stream_finish_timeout = std::chrono::milliseconds(1000);
    tuning.stream_wait_warn_interval = std::chrono::milliseconds(50);

    // OnDone arrives inside the bounded wait: the normal path must not gain
    // any warning from the periodic reporting.
    std::string log;
    run_ping_finish_with_held_on_done(*mock_agent_service_, tuning, std::chrono::milliseconds(0), &log);
    EXPECT_EQ(log.find("still waiting"), std::string::npos) << log;
    EXPECT_EQ(log.find("[warning]"), std::string::npos) << log;
}

TEST_F(GrpcMockTest, GrpcAgentLogsPeriodicallyWhileWaitingForRegistration) {
    auto cfg = mock_agent_service_->mutableConfig();
    cfg->collector.agent_info.send_retry_interval_ms = 10;
    GrpcClientTuning tuning;
    tuning.registration_wait_log_interval = std::chrono::milliseconds(50);

    CountingAgentInfoGrpcAgent grpc_agent(cfg, SEND_FAIL, tuning);
    grpc_agent.setAgentService(mock_agent_service_.get());

    std::string log;
    {
        LogCapture capture;
        ScopedWorker registrar([this] { mock_agent_service_->setExiting(true); },
                               [&grpc_agent] { grpc_agent.registerAgentWithRetry(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        mock_agent_service_->setExiting(true);
        registrar.join();
        log = capture.text();
    }
    // Timing slack: at least half of the ~6 intervals must have been reported.
    EXPECT_GE(LogCapture::count(log, "still waiting for agent registration"), 3u) << log;
}

// A rejected registration can be permanent (wrong app name, unsupported agent
// id) and retries forever, so both the per-attempt line and the periodic wait
// line must say the collector answered rather than that it was unreachable —
// they point at opposite fixes.
TEST_F(GrpcMockTest, GrpcAgentRegistrationWaitReportsCollectorRejection) {
    auto cfg = mock_agent_service_->mutableConfig();
    cfg->collector.agent_info.send_retry_interval_ms = 10;
    GrpcClientTuning tuning;
    tuning.registration_wait_log_interval = std::chrono::milliseconds(50);

    TestableGrpcAgent grpc_agent(mock_agent_service_.get(), tuning);
    auto mock_agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
    ON_CALL(*mock_agent_stub, RequestAgentInfo(_, _, _))
        .WillByDefault(DoAll(SetArgPointee<2>(rejected_result("unknown application name")),
                             Return(grpc::Status::OK)));
    grpc_agent.setMockAgentStub(std::move(mock_agent_stub));

    std::string log;
    {
        LogCapture capture;
        ScopedWorker registrar([this] { mock_agent_service_->setExiting(true); },
                               [&grpc_agent] { grpc_agent.registerAgentWithRetry(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        mock_agent_service_->setExiting(true);
        registrar.join();
        log = capture.text();
    }
    EXPECT_NE(log.find("collector rejected it (PResult.success=false)"), std::string::npos) << log;
    EXPECT_NE(log.find("unknown application name"), std::string::npos) << log;
    EXPECT_NE(log.find("collector rejected the registration"), std::string::npos) << log;
    EXPECT_EQ(log.find("collector unreachable"), std::string::npos) << log;
}

} // namespace pinpoint
