/*
 * Copyright 2020-present NAVER Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "test/it/mock_collector.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/security/server_credentials.h>

namespace pinpoint::test {
namespace {

RpcMetadata copy_metadata(const grpc::ServerContext& context) {
    RpcMetadata metadata;
    for (const auto& [key, value] : context.client_metadata()) {
        metadata.values.emplace(
            std::string(key.data(), key.size()),
            std::string(value.data(), value.size()));
    }
    return metadata;
}

void set_success(v1::PResult* response) {
    response->set_success(true);
}

enum class FaultKind { GrpcError, Timeout, ApplicationError };

struct FaultAction {
    FaultKind kind;
    grpc::StatusCode code;
    std::string message;
    size_t after_messages;
};

}  // namespace

std::optional<std::string> RpcMetadata::value(std::string_view key) const {
    const auto it = values.find(std::string(key));
    if (it == values.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<std::string> RpcMetadata::all(std::string_view key) const {
    std::vector<std::string> result;
    const auto [first, last] = values.equal_range(std::string(key));
    for (auto it = first; it != last; ++it) {
        result.push_back(it->second);
    }
    return result;
}

struct MockCollector::Impl {
    struct AgentService final : v1::Agent::Service {
        explicit AgentService(Impl& owner) : owner_(owner) {}

        grpc::Status RequestAgentInfo(grpc::ServerContext* context,
                                      const v1::PAgentInfo* request,
                                      v1::PResult* response) override {
            owner_.record(&CollectorSnapshot::agent_infos, *request, copy_metadata(*context));
            return owner_.complete_unary(CollectorRpc::AgentInfo, context, response);
        }

        grpc::Status PingSession(
                grpc::ServerContext* context,
                grpc::ServerReaderWriter<v1::PPing, v1::PPing>* stream) override {
            const auto metadata = copy_metadata(*context);
            owner_.record_stream(&CollectorSnapshot::ping_streams, metadata);

            if (auto fault = owner_.apply_stream_fault(
                    CollectorRpc::PingSession, context, 0)) {
                return *fault;
            }

            v1::PPing ping;
            size_t message_count = 0;
            while (stream->Read(&ping)) {
                owner_.record(&CollectorSnapshot::pings, ping, metadata);
                ++message_count;
                if (auto fault = owner_.apply_stream_fault(
                        CollectorRpc::PingSession, context, message_count)) {
                    return *fault;
                }
                if (!stream->Write(ping)) {
                    break;
                }
                ping.Clear();
            }
            return owner_.complete_stream(CollectorRpc::PingSession);
        }

    private:
        Impl& owner_;
    };

    struct MetadataService final : v1::Metadata::Service {
        explicit MetadataService(Impl& owner) : owner_(owner) {}

        grpc::Status RequestSqlMetaData(grpc::ServerContext* context,
                                        const v1::PSqlMetaData* request,
                                        v1::PResult* response) override {
            return receive(CollectorRpc::SqlMetadata, context, request, response,
                           &CollectorSnapshot::sql_metadata);
        }

        grpc::Status RequestSqlUidMetaData(grpc::ServerContext* context,
                                           const v1::PSqlUidMetaData* request,
                                           v1::PResult* response) override {
            return receive(CollectorRpc::SqlUidMetadata, context, request, response,
                           &CollectorSnapshot::sql_uid_metadata);
        }

        grpc::Status RequestApiMetaData(grpc::ServerContext* context,
                                        const v1::PApiMetaData* request,
                                        v1::PResult* response) override {
            return receive(CollectorRpc::ApiMetadata, context, request, response,
                           &CollectorSnapshot::api_metadata);
        }

        grpc::Status RequestStringMetaData(grpc::ServerContext* context,
                                           const v1::PStringMetaData* request,
                                           v1::PResult* response) override {
            return receive(CollectorRpc::StringMetadata, context, request, response,
                           &CollectorSnapshot::string_metadata);
        }

        grpc::Status RequestExceptionMetaData(grpc::ServerContext* context,
                                              const v1::PExceptionMetaData* request,
                                              v1::PResult* response) override {
            return receive(CollectorRpc::ExceptionMetadata, context, request, response,
                           &CollectorSnapshot::exception_metadata);
        }

    private:
        template <typename Message>
        grpc::Status receive(
                CollectorRpc rpc,
                grpc::ServerContext* context,
                const Message* request,
                v1::PResult* response,
                std::vector<Received<Message>> CollectorSnapshot::*member) {
            owner_.record(member, *request, copy_metadata(*context));
            return owner_.complete_unary(rpc, context, response);
        }

        Impl& owner_;
    };

    struct SpanService final : v1::Span::Service {
        explicit SpanService(Impl& owner) : owner_(owner) {}

        grpc::Status SendSpan(grpc::ServerContext* context,
                              grpc::ServerReader<v1::PSpanMessage>* reader,
                              google::protobuf::Empty*) override {
            const auto metadata = copy_metadata(*context);
            owner_.record_stream(&CollectorSnapshot::span_streams, metadata);

            if (auto fault = owner_.apply_stream_fault(
                    CollectorRpc::SendSpan, context, 0)) {
                return *fault;
            }

            v1::PSpanMessage message;
            size_t message_count = 0;
            while (reader->Read(&message)) {
                owner_.record(&CollectorSnapshot::span_messages, message, metadata);
                ++message_count;
                if (auto fault = owner_.apply_stream_fault(
                        CollectorRpc::SendSpan, context, message_count)) {
                    return *fault;
                }
                message.Clear();
            }
            return owner_.complete_stream(CollectorRpc::SendSpan);
        }

        grpc::Status SendSpanBatch(grpc::ServerContext* context,
                                   const v1::PSpanMessageBatch* request,
                                   v1::PSpanResultBatch*) override {
            owner_.record(&CollectorSnapshot::span_batches, *request, copy_metadata(*context));
            return owner_.complete_unary(CollectorRpc::SendSpanBatch, context, nullptr);
        }

    private:
        Impl& owner_;
    };

    struct StatService final : v1::Stat::Service {
        explicit StatService(Impl& owner) : owner_(owner) {}

        grpc::Status SendAgentStat(grpc::ServerContext* context,
                                   grpc::ServerReader<v1::PStatMessage>* reader,
                                   google::protobuf::Empty*) override {
            const auto metadata = copy_metadata(*context);
            owner_.record_stream(&CollectorSnapshot::stat_streams, metadata);

            if (auto fault = owner_.apply_stream_fault(
                    CollectorRpc::SendAgentStat, context, 0)) {
                return *fault;
            }

            v1::PStatMessage stat;
            size_t message_count = 0;
            while (reader->Read(&stat)) {
                owner_.record(&CollectorSnapshot::stats, stat, metadata);
                ++message_count;
                if (auto fault = owner_.apply_stream_fault(
                        CollectorRpc::SendAgentStat, context, message_count)) {
                    return *fault;
                }
                stat.Clear();
            }
            return owner_.complete_stream(CollectorRpc::SendAgentStat);
        }

    private:
        Impl& owner_;
    };

    struct CommandService final : v1::ProfilerCommandService::Service {
        explicit CommandService(Impl& owner) : owner_(owner) {}

        grpc::Status HandleCommand(
                grpc::ServerContext* context,
                grpc::ServerReaderWriter<v1::PCmdRequest, v1::PCmdMessage>* stream) override {
            return owner_.command_stream(
                CollectorRpc::HandleCommand, context, stream, false);
        }

        grpc::Status HandleCommandV2(
                grpc::ServerContext* context,
                grpc::ServerReaderWriter<v1::PCmdRequest, v1::PCmdMessage>* stream) override {
            return owner_.command_stream(
                CollectorRpc::HandleCommandV2, context, stream, true);
        }

        grpc::Status CommandEcho(grpc::ServerContext* context,
                                 const v1::PCmdEchoResponse* request,
                                 google::protobuf::Empty*) override {
            owner_.record(&CollectorSnapshot::echo_responses, *request, copy_metadata(*context));
            return owner_.complete_unary(CollectorRpc::CommandEcho, context, nullptr);
        }

        grpc::Status CommandStreamActiveThreadCount(
                grpc::ServerContext* context,
                grpc::ServerReader<v1::PCmdActiveThreadCountRes>* reader,
                google::protobuf::Empty*) override {
            const auto metadata = copy_metadata(*context);
            owner_.record_stream(&CollectorSnapshot::active_thread_count_streams, metadata);

            if (auto fault = owner_.apply_stream_fault(
                    CollectorRpc::CommandStreamActiveThreadCount, context, 0)) {
                return *fault;
            }

            v1::PCmdActiveThreadCountRes response;
            size_t message_count = 0;
            while (reader->Read(&response)) {
                owner_.record(&CollectorSnapshot::active_thread_count_responses, response, metadata);
                ++message_count;
                if (auto fault = owner_.apply_stream_fault(
                        CollectorRpc::CommandStreamActiveThreadCount,
                        context, message_count)) {
                    return *fault;
                }
                response.Clear();
            }
            return owner_.complete_stream(
                CollectorRpc::CommandStreamActiveThreadCount);
        }

        grpc::Status CommandActiveThreadDump(
                grpc::ServerContext* context,
                const v1::PCmdActiveThreadDumpRes* request,
                google::protobuf::Empty*) override {
            owner_.record(&CollectorSnapshot::active_thread_dump_responses,
                          *request, copy_metadata(*context));
            return owner_.complete_unary(
                CollectorRpc::CommandActiveThreadDump, context, nullptr);
        }

        grpc::Status CommandActiveThreadLightDump(
                grpc::ServerContext* context,
                const v1::PCmdActiveThreadLightDumpRes* request,
                google::protobuf::Empty*) override {
            owner_.record(&CollectorSnapshot::active_thread_light_dump_responses,
                          *request, copy_metadata(*context));
            return owner_.complete_unary(
                CollectorRpc::CommandActiveThreadLightDump, context, nullptr);
        }

    private:
        Impl& owner_;
    };

    Impl()
        : agent_service(*this),
          metadata_service(*this),
          span_service(*this),
          stat_service(*this),
          command_service(*this) {}

    template <typename Message>
    void record(std::vector<Received<Message>> CollectorSnapshot::*member,
                const Message& message,
                const RpcMetadata& metadata) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            (records.*member).push_back(Received<Message>{message, metadata});
        }
        cv.notify_all();
    }

    void record_stream(std::vector<RpcMetadata> CollectorSnapshot::*member,
                       const RpcMetadata& metadata) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            (records.*member).push_back(metadata);
        }
        cv.notify_all();
    }

    void record_result(CollectorRpc rpc,
                       grpc::StatusCode status_code,
                       bool response_success,
                       std::string message) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            records.rpc_results.push_back(
                RpcResult{rpc, status_code, response_success, std::move(message)});
        }
        cv.notify_all();
    }

    std::optional<FaultAction> take_ready_fault(CollectorRpc rpc,
                                                 size_t message_count) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = faults.find(rpc);
        if (it == faults.end() || it->second.empty() ||
            it->second.front().after_messages > message_count) {
            return std::nullopt;
        }

        auto fault = std::move(it->second.front());
        it->second.pop_front();
        if (it->second.empty()) {
            faults.erase(it);
        }
        return fault;
    }

    void wait_for_cancellation(const grpc::ServerContext& context) {
        while (!context.IsCancelled()) {
            std::unique_lock<std::mutex> lock(mutex);
            if (stopping) {
                return;
            }
            cv.wait_for(lock, std::chrono::milliseconds(10));
        }
    }

    grpc::Status complete_unary(CollectorRpc rpc,
                                grpc::ServerContext* context,
                                v1::PResult* response) {
        const auto fault = take_ready_fault(rpc, 0);
        if (!fault.has_value()) {
            if (response != nullptr) {
                set_success(response);
            }
            record_result(rpc, grpc::StatusCode::OK, true, {});
            return grpc::Status::OK;
        }

        if (fault->kind == FaultKind::ApplicationError && response != nullptr) {
            response->set_success(false);
            response->set_message(fault->message);
            record_result(rpc, grpc::StatusCode::OK, false, fault->message);
            return grpc::Status::OK;
        }

        if (fault->kind == FaultKind::Timeout) {
            wait_for_cancellation(*context);
            record_result(rpc, grpc::StatusCode::DEADLINE_EXCEEDED,
                          false, fault->message);
            return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                                fault->message);
        }

        const auto code = fault->kind == FaultKind::ApplicationError
            ? grpc::StatusCode::FAILED_PRECONDITION
            : fault->code;
        record_result(rpc, code, false, fault->message);
        return grpc::Status(code, fault->message);
    }

    std::optional<grpc::Status> apply_stream_fault(
            CollectorRpc rpc,
            grpc::ServerContext* context,
            size_t message_count) {
        const auto fault = take_ready_fault(rpc, message_count);
        if (!fault.has_value()) {
            return std::nullopt;
        }

        if (fault->kind == FaultKind::Timeout) {
            wait_for_cancellation(*context);
            record_result(rpc, grpc::StatusCode::DEADLINE_EXCEEDED,
                          false, fault->message);
            return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                                fault->message);
        }

        const auto code = fault->kind == FaultKind::ApplicationError
            ? grpc::StatusCode::FAILED_PRECONDITION
            : fault->code;
        record_result(rpc, code, false, fault->message);
        return grpc::Status(code, fault->message);
    }

    grpc::Status complete_stream(CollectorRpc rpc) {
        record_result(rpc, grpc::StatusCode::OK, true, {});
        return grpc::Status::OK;
    }

    grpc::Status command_stream(
            CollectorRpc rpc,
            grpc::ServerContext* context,
            grpc::ServerReaderWriter<v1::PCmdRequest, v1::PCmdMessage>* stream,
            bool v2) {
        const auto metadata = copy_metadata(*context);
        record_stream(v2 ? &CollectorSnapshot::command_streams_v2
                         : &CollectorSnapshot::command_streams,
                      metadata);

        // Command streams have no deadline and normally carry no client-side
        // messages, so faults are applied at stream establishment time.
        if (auto fault = apply_stream_fault(rpc, context, 0)) {
            return *fault;
        }

        // The sync API permits one reader and one writer concurrently. Keeping
        // the read side live is important: unsupported commands are answered
        // with PCmdMessage.failMessage on this stream and must be recorded too.
        std::atomic<bool> reader_done{false};
        std::thread reader([this, stream, metadata, &reader_done] {
            v1::PCmdMessage message;
            while (stream->Read(&message)) {
                record(&CollectorSnapshot::command_stream_messages, message, metadata);
                message.Clear();
            }
            reader_done = true;
            cv.notify_all();
        });

        while (!context->IsCancelled() && !reader_done.load()) {
            std::optional<v1::PCmdRequest> command;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(25), [this, &reader_done] {
                    return stopping || reader_done.load() || !commands.empty();
                });
                if (stopping || reader_done.load()) {
                    break;
                }
                if (!commands.empty()) {
                    command.emplace(std::move(commands.front()));
                    commands.pop_front();
                }
            }

            if (command.has_value() && !stream->Write(*command)) {
                break;
            }
        }

        // Unblock a read that is still pending when the server or write side
        // decides to finish. A handler invoking TryCancel must return CANCELLED.
        if (!reader_done.load()) {
            context->TryCancel();
        }
        if (reader.joinable()) {
            reader.join();
        }
        if (context->IsCancelled()) {
            record_result(rpc, grpc::StatusCode::CANCELLED,
                          false, "command stream closed");
            return grpc::Status(grpc::StatusCode::CANCELLED,
                                "command stream closed");
        }
        return complete_stream(rpc);
    }

    bool start() {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
        if (agent_server || span_server || stat_server) {
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = false;
        }

        if (!start_endpoint_locked(CollectorEndpoint::Agent) ||
            !start_endpoint_locked(CollectorEndpoint::Span) ||
            !start_endpoint_locked(CollectorEndpoint::Stat)) {
            shutdown_locked();
            return false;
        }
        return true;
    }

    bool start_endpoint(CollectorEndpoint endpoint) {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = false;
        }
        return start_endpoint_locked(endpoint);
    }

    bool start_endpoint_locked(CollectorEndpoint endpoint) {
        grpc::ServerBuilder builder;
        std::unique_ptr<grpc::Server>* server = nullptr;
        int* port = nullptr;

        switch (endpoint) {
        case CollectorEndpoint::Agent:
            if (agent_server) {
                return true;
            }
            builder.RegisterService(&agent_service);
            builder.RegisterService(&metadata_service);
            builder.RegisterService(&command_service);
            server = &agent_server;
            port = &agent_port;
            break;
        case CollectorEndpoint::Span:
            if (span_server) {
                return true;
            }
            builder.RegisterService(&span_service);
            server = &span_server;
            port = &span_port;
            break;
        case CollectorEndpoint::Stat:
            if (stat_server) {
                return true;
            }
            builder.RegisterService(&stat_service);
            server = &stat_server;
            port = &stat_port;
            break;
        }

        const auto requested_port = *port;
        const auto address = host_name + ":" +
            (requested_port == 0 ? std::string("0") : std::to_string(requested_port));
        int selected_port = 0;
        builder.AddListeningPort(address, grpc::InsecureServerCredentials(),
                                 &selected_port);
        *server = builder.BuildAndStart();
        if (!*server || selected_port == 0 ||
            (requested_port != 0 && selected_port != requested_port)) {
            server->reset();
            return false;
        }
        *port = selected_port;
        return true;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
        shutdown_locked();
    }

    bool stop_endpoint(CollectorEndpoint endpoint) {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
        stop_endpoint_locked(endpoint, true);
        return true;
    }

    void stop_endpoint_locked(CollectorEndpoint endpoint, bool immediate) {
        std::unique_ptr<grpc::Server>* server = nullptr;
        switch (endpoint) {
        case CollectorEndpoint::Agent:
            server = &agent_server;
            break;
        case CollectorEndpoint::Span:
            server = &span_server;
            break;
        case CollectorEndpoint::Stat:
            server = &stat_server;
            break;
        }

        if (!*server) {
            return;
        }
        const auto deadline = immediate
            ? std::chrono::system_clock::now()
            : std::chrono::system_clock::now() + std::chrono::seconds(3);
        (*server)->Shutdown(deadline);
        (*server)->Wait();
        server->reset();
    }

    void shutdown_locked() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        cv.notify_all();

        stop_endpoint_locked(CollectorEndpoint::Agent, false);
        stop_endpoint_locked(CollectorEndpoint::Span, false);
        stop_endpoint_locked(CollectorEndpoint::Stat, false);
        agent_port = 0;
        span_port = 0;
        stat_port = 0;
    }

    CollectorSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        return records;
    }

    bool wait_for(const std::function<bool(const CollectorSnapshot&)>& predicate,
                  std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [this, &predicate] {
            return predicate(records);
        });
    }

    void send_command(v1::PCmdRequest command) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            commands.push_back(std::move(command));
        }
        cv.notify_all();
    }

    void add_fault(CollectorRpc rpc, FaultAction fault) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            faults[rpc].push_back(std::move(fault));
        }
        cv.notify_all();
    }

    const std::string host_name{"127.0.0.1"};
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    std::mutex lifecycle_mutex;
    CollectorSnapshot records;
    std::deque<v1::PCmdRequest> commands;
    std::map<CollectorRpc, std::deque<FaultAction>> faults;
    bool stopping{false};

    int agent_port{0};
    int span_port{0};
    int stat_port{0};
    std::unique_ptr<grpc::Server> agent_server;
    std::unique_ptr<grpc::Server> span_server;
    std::unique_ptr<grpc::Server> stat_server;

    AgentService agent_service;
    MetadataService metadata_service;
    SpanService span_service;
    StatService stat_service;
    CommandService command_service;
};

MockCollector::MockCollector() : impl_(std::make_unique<Impl>()) {}

MockCollector::~MockCollector() {
    Shutdown();
}

bool MockCollector::Start() {
    return impl_->start();
}

void MockCollector::Shutdown() {
    if (impl_) {
        impl_->shutdown();
    }
}

bool MockCollector::StopEndpoint(CollectorEndpoint endpoint) {
    return impl_->stop_endpoint(endpoint);
}

bool MockCollector::StartEndpoint(CollectorEndpoint endpoint) {
    return impl_->start_endpoint(endpoint);
}

const std::string& MockCollector::host() const {
    return impl_->host_name;
}

int MockCollector::agent_port() const {
    return impl_->agent_port;
}

int MockCollector::span_port() const {
    return impl_->span_port;
}

int MockCollector::stat_port() const {
    return impl_->stat_port;
}

CollectorSnapshot MockCollector::snapshot() const {
    return impl_->snapshot();
}

bool MockCollector::WaitFor(
        const std::function<bool(const CollectorSnapshot&)>& predicate,
        std::chrono::milliseconds timeout) const {
    return impl_->wait_for(predicate, timeout);
}

void MockCollector::FailNext(CollectorRpc rpc,
                             grpc::StatusCode code,
                             std::string message,
                             size_t after_messages) {
    impl_->add_fault(rpc, FaultAction{
        FaultKind::GrpcError, code, std::move(message), after_messages});
}

void MockCollector::TimeoutNext(CollectorRpc rpc, size_t after_messages) {
    impl_->add_fault(rpc, FaultAction{
        FaultKind::Timeout, grpc::StatusCode::DEADLINE_EXCEEDED,
        "injected timeout", after_messages});
}

void MockCollector::RejectNext(CollectorRpc rpc, std::string message) {
    impl_->add_fault(rpc, FaultAction{
        FaultKind::ApplicationError, grpc::StatusCode::OK,
        std::move(message), 0});
}

void MockCollector::SendCommand(v1::PCmdRequest request) {
    impl_->send_command(std::move(request));
}

void MockCollector::SendEchoCommand(int32_t request_id, std::string message) {
    v1::PCmdRequest request;
    request.set_requestid(request_id);
    request.mutable_commandecho()->set_message(std::move(message));
    SendCommand(std::move(request));
}

void MockCollector::SendActiveThreadCountCommand(int32_t request_id) {
    v1::PCmdRequest request;
    request.set_requestid(request_id);
    request.mutable_commandactivethreadcount();
    SendCommand(std::move(request));
}

void MockCollector::SendActiveThreadDumpCommand(int32_t request_id, int32_t limit) {
    v1::PCmdRequest request;
    request.set_requestid(request_id);
    request.mutable_commandactivethreaddump()->set_limit(limit);
    SendCommand(std::move(request));
}

}  // namespace pinpoint::test
