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

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "v1/Service.grpc.pb.h"

namespace pinpoint::test {

/** Collector endpoint groups matching the agent's three configured ports. */
enum class CollectorEndpoint { Agent, Span, Stat };

/** Individual RPCs that can be targeted by deterministic fault injection. */
enum class CollectorRpc {
    AgentInfo,
    PingSession,
    SqlMetadata,
    SqlUidMetadata,
    ApiMetadata,
    StringMetadata,
    ExceptionMetadata,
    SendSpan,
    SendSpanBatch,
    SendAgentStat,
    HandleCommand,
    HandleCommandV2,
    CommandEcho,
    CommandStreamActiveThreadCount,
    CommandActiveThreadDump,
};

/** A copy of the client metadata attached to one gRPC call. */
struct RpcMetadata {
    std::multimap<std::string, std::string> values;
    /** grpc::ServerContext::peer(): "ipv4:127.0.0.1:<port>". The client's
     *  ephemeral port identifies the connection the call arrived on, so a
     *  changed peer proves the agent moved to a new channel. */
    std::string peer;

    std::optional<std::string> value(std::string_view key) const;
    std::vector<std::string> all(std::string_view key) const;
};

/** A protobuf received by the mock collector and the headers of its call. */
template <typename Message>
struct Received {
    Message message;
    RpcMetadata metadata;
};

/** Result returned by one mock service handler. */
struct RpcResult {
    CollectorRpc rpc;
    grpc::StatusCode status_code;
    bool response_success;
    std::string message;
};

/**
 * Immutable copy of everything received by the mock collector so far.
 *
 * Stream-call vectors are recorded when the RPC opens, even when the stream
 * never carries a message. Message vectors contain a full protobuf copy for
 * every request/write received by the server.
 */
struct CollectorSnapshot {
    std::vector<RpcResult> rpc_results;

    std::vector<Received<v1::PAgentInfo>> agent_infos;
    std::vector<RpcMetadata> ping_streams;
    std::vector<Received<v1::PPing>> pings;

    std::vector<Received<v1::PSqlMetaData>> sql_metadata;
    std::vector<Received<v1::PSqlUidMetaData>> sql_uid_metadata;
    std::vector<Received<v1::PApiMetaData>> api_metadata;
    std::vector<Received<v1::PStringMetaData>> string_metadata;
    std::vector<Received<v1::PExceptionMetaData>> exception_metadata;

    std::vector<Received<v1::PSpanMessage>> span_messages;
    std::vector<Received<v1::PSpanMessageBatch>> span_batches;

    std::vector<RpcMetadata> stat_streams;
    std::vector<Received<v1::PStatMessage>> stats;

    std::vector<RpcMetadata> command_streams;
    std::vector<RpcMetadata> command_streams_v2;
    std::vector<Received<v1::PCmdMessage>> command_stream_messages;
    std::vector<Received<v1::PCmdEchoResponse>> echo_responses;
    std::vector<Received<v1::PCmdActiveThreadCountRes>> active_thread_count_responses;
};

/**
 * In-process Pinpoint collector used by integration tests.
 *
 * It exposes the five services from pinpoint-grpc-idl on the same three-port
 * topology as a real collector:
 *   - Agent + Metadata + ProfilerCommandService on agent_port()
 *   - Span on span_port()
 *   - Stat on stat_port()
 *
 * Every server binds to 127.0.0.1:0, so the operating system selects an
 * ephemeral port. All records and waits are thread-safe.
 */
class MockCollector final {
public:
    MockCollector();
    ~MockCollector();

    MockCollector(const MockCollector&) = delete;
    MockCollector& operator=(const MockCollector&) = delete;

    bool Start();
    void Shutdown();

    /** Abruptly stops or restarts one real listening endpoint on the same port. */
    bool StopEndpoint(CollectorEndpoint endpoint);
    bool StartEndpoint(CollectorEndpoint endpoint);

    /**
     * Enters a sustained collector outage: every stream in flight is
     * cancelled and every subsequent RPC on all three endpoints fails with
     * @p code until EndOutage() is called. Unlike FailNext(), the fault is
     * not consumed per call; unlike StopEndpoint(), the listening ports stay
     * open, so the agent observes RPC-level failures (an unhealthy
     * collector) rather than connection refusals (a dead host). Queued
     * FailNext/TimeoutNext/RejectNext faults are left untouched and apply
     * again after the outage ends. Every failed call is still recorded in
     * CollectorSnapshot::rpc_results (and unary request protobufs are still
     * captured), so tests can observe the agent's retry attempts.
     */
    void BeginOutage(grpc::StatusCode code = grpc::StatusCode::UNAVAILABLE,
                     std::string message = "injected collector outage");

    /** Ends a BeginOutage() period; subsequent RPCs behave normally again. */
    void EndOutage();

    const std::string& host() const;
    int agent_port() const;
    int span_port() const;
    int stat_port() const;

    CollectorSnapshot snapshot() const;

    /** Waits until @p predicate matches a coherent collector snapshot. */
    bool WaitFor(const std::function<bool(const CollectorSnapshot&)>& predicate,
                 std::chrono::milliseconds timeout) const;

    /**
     * Returns a gRPC error from the next matching RPC or stream.
     * For streams, @p after_messages delays the error until that many client
     * messages have been recorded; zero rejects the stream as it opens.
     */
    void FailNext(CollectorRpc rpc,
                  grpc::StatusCode code = grpc::StatusCode::UNAVAILABLE,
                  std::string message = "injected gRPC failure",
                  size_t after_messages = 0);

    /**
     * Withholds the next response until the client deadline/cancellation.
     * @p after_messages has the same stream semantics as FailNext().
     */
    void TimeoutNext(CollectorRpc rpc, size_t after_messages = 0);

    /** Returns grpc::Status::OK with PResult.success=false on the next unary RPC. */
    void RejectNext(CollectorRpc rpc, std::string message = "injected application failure");

    /** Queues a collector-originated request for the active command stream. */
    void SendCommand(v1::PCmdRequest request);
    void SendEchoCommand(int32_t request_id, std::string message);
    void SendActiveThreadCountCommand(int32_t request_id);
    void SendActiveThreadDumpCommand(int32_t request_id, int32_t limit = 1);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pinpoint::test
