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

// Counting-only collector for the version-comparison benchmark.
//
// test/it/mock_collector.h cannot be reused here: it keeps a full protobuf copy
// of every message it receives so integration tests can assert on payloads. At
// benchmark volumes that retention would dominate memory and perturb the very
// numbers being measured. This collector accepts the same five services on the
// same three-port topology, counts what arrives, and drops it.
//
// Both agent versions only record real spans once AgentInfo registration
// succeeds, so this process must be running before either benchmark starts.

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include "v1/Service.grpc.pb.h"

namespace {

    std::atomic<bool> g_stop{false};

    void handle_signal(int) { g_stop.store(true); }

    struct Counters {
        std::atomic<uint64_t> agent_infos{0};
        std::atomic<uint64_t> metadata{0};
        std::atomic<uint64_t> span_messages{0};
        std::atomic<uint64_t> span_batches{0};
        std::atomic<uint64_t> spans_in_batches{0};
        std::atomic<uint64_t> stats{0};
    };

    Counters g_counters;

    class AgentService final : public v1::Agent::Service {
    public:
        grpc::Status RequestAgentInfo(grpc::ServerContext*, const v1::PAgentInfo* request,
                                      v1::PResult* response) override {
            g_counters.agent_infos.fetch_add(1, std::memory_order_relaxed);
            // The agent id and application name travel as call metadata, not in
            // the message body.
            std::cerr << "[collector] AgentInfo from host=" << request->hostname()
                      << " pid=" << request->pid()
                      << " version=" << request->agentversion() << std::endl;
            response->set_success(true);
            return grpc::Status::OK;
        }

        grpc::Status PingSession(grpc::ServerContext*,
                                 grpc::ServerReaderWriter<v1::PPing, v1::PPing>* stream) override {
            v1::PPing ping;
            while (stream->Read(&ping)) {
                stream->Write(ping);
            }
            return grpc::Status::OK;
        }
    };

    class MetadataService final : public v1::Metadata::Service {
    public:
        grpc::Status RequestSqlMetaData(grpc::ServerContext*, const v1::PSqlMetaData*,
                                        v1::PResult* response) override {
            return ok(response);
        }
        grpc::Status RequestSqlUidMetaData(grpc::ServerContext*, const v1::PSqlUidMetaData*,
                                           v1::PResult* response) override {
            return ok(response);
        }
        grpc::Status RequestApiMetaData(grpc::ServerContext*, const v1::PApiMetaData*,
                                        v1::PResult* response) override {
            return ok(response);
        }
        grpc::Status RequestStringMetaData(grpc::ServerContext*, const v1::PStringMetaData*,
                                           v1::PResult* response) override {
            return ok(response);
        }
        grpc::Status RequestExceptionMetaData(grpc::ServerContext*, const v1::PExceptionMetaData*,
                                              v1::PResult* response) override {
            return ok(response);
        }

    private:
        static grpc::Status ok(v1::PResult* response) {
            g_counters.metadata.fetch_add(1, std::memory_order_relaxed);
            response->set_success(true);
            return grpc::Status::OK;
        }
    };

    // v1.1.0 streams spans through SendSpan; the current agent batches them
    // through the unary SendSpanBatch. Both are served so one collector process
    // can host both benchmark runs.
    class SpanService final : public v1::Span::Service {
    public:
        grpc::Status SendSpan(grpc::ServerContext*,
                              grpc::ServerReader<v1::PSpanMessage>* reader,
                              google::protobuf::Empty*) override {
            v1::PSpanMessage message;
            uint64_t received = 0;
            while (reader->Read(&message)) {
                ++received;
            }
            g_counters.span_messages.fetch_add(received, std::memory_order_relaxed);
            return grpc::Status::OK;
        }

        grpc::Status SendSpanBatch(grpc::ServerContext*, const v1::PSpanMessageBatch* request,
                                   v1::PSpanResultBatch*) override {
            g_counters.span_batches.fetch_add(1, std::memory_order_relaxed);
            g_counters.spans_in_batches.fetch_add(static_cast<uint64_t>(request->span_size()),
                                                 std::memory_order_relaxed);
            return grpc::Status::OK;
        }
    };

    class StatService final : public v1::Stat::Service {
    public:
        grpc::Status SendAgentStat(grpc::ServerContext*,
                                   grpc::ServerReader<v1::PStatMessage>* reader,
                                   google::protobuf::Empty*) override {
            v1::PStatMessage message;
            uint64_t received = 0;
            while (reader->Read(&message)) {
                ++received;
            }
            g_counters.stats.fetch_add(received, std::memory_order_relaxed);
            return grpc::Status::OK;
        }
    };

    // Kept open and idle. Leaving it unimplemented would make the agent retry
    // the command stream in a loop and burn CPU next to the measured threads.
    class CommandService final : public v1::ProfilerCommandService::Service {
    public:
        grpc::Status HandleCommand(grpc::ServerContext*,
                                   grpc::ServerReaderWriter<v1::PCmdRequest, v1::PCmdMessage>* stream) override {
            return drain(stream);
        }

        grpc::Status HandleCommandV2(grpc::ServerContext*,
                                     grpc::ServerReaderWriter<v1::PCmdRequest, v1::PCmdMessage>* stream) override {
            return drain(stream);
        }

    private:
        static grpc::Status drain(grpc::ServerReaderWriter<v1::PCmdRequest, v1::PCmdMessage>* stream) {
            v1::PCmdMessage message;
            while (stream->Read(&message)) {
                // Discard: no commands are issued during a benchmark run.
            }
            return grpc::Status::OK;
        }
    };

    std::unique_ptr<grpc::Server> build_server(const std::string& address, int* bound_port,
                                               const std::vector<grpc::Service*>& services) {
        grpc::ServerBuilder builder;
        builder.AddListeningPort(address, grpc::InsecureServerCredentials(), bound_port);
        for (auto* service : services) {
            builder.RegisterService(service);
        }
        return builder.BuildAndStart();
    }

}  // namespace

int main(int argc, char** argv) {
    int agent_port = 0;
    int span_port = 0;
    int stat_port = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string{}; };
        if (arg == "--agent-port") {
            agent_port = std::stoi(next());
        } else if (arg == "--span-port") {
            span_port = std::stoi(next());
        } else if (arg == "--stat-port") {
            stat_port = std::stoi(next());
        }
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    AgentService agent_service;
    MetadataService metadata_service;
    CommandService command_service;
    SpanService span_service;
    StatService stat_service;

    int agent_bound = 0;
    int span_bound = 0;
    int stat_bound = 0;

    auto agent_server = build_server("127.0.0.1:" + std::to_string(agent_port), &agent_bound,
                                     {&agent_service, &metadata_service, &command_service});
    auto span_server = build_server("127.0.0.1:" + std::to_string(span_port), &span_bound,
                                    {&span_service});
    auto stat_server = build_server("127.0.0.1:" + std::to_string(stat_port), &stat_bound,
                                   {&stat_service});

    if (agent_server == nullptr || span_server == nullptr || stat_server == nullptr) {
        std::cerr << "[collector] failed to start one or more endpoints" << std::endl;
        return 1;
    }

    // Consumed by run_compare.sh to configure both benchmark binaries.
    std::cout << "AGENT_PORT=" << agent_bound << std::endl;
    std::cout << "SPAN_PORT=" << span_bound << std::endl;
    std::cout << "STAT_PORT=" << stat_bound << std::endl;
    std::cout << "READY" << std::endl;

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Parseable tally so the runner can check that the benchmark's spans
    // actually arrived. A version whose sender falls behind drops spans on a
    // cheap path instead of serializing them, which would measure as a speedup.
    std::cout << "COUNTERS"
              << '\t' << g_counters.agent_infos.load()
              << '\t' << g_counters.metadata.load()
              << '\t' << g_counters.span_messages.load()
              << '\t' << g_counters.span_batches.load()
              << '\t' << g_counters.spans_in_batches.load()
              << '\t' << g_counters.stats.load() << std::endl;

    std::cerr << "[collector] agent_infos=" << g_counters.agent_infos.load()
              << " metadata=" << g_counters.metadata.load()
              << " span_messages=" << g_counters.span_messages.load()
              << " span_batches=" << g_counters.span_batches.load()
              << " spans_in_batches=" << g_counters.spans_in_batches.load()
              << " stats=" << g_counters.stats.load() << std::endl;

    agent_server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    span_server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    stat_server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    return 0;
}
