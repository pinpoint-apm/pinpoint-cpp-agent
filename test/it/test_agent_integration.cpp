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

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "pinpoint/tracer.h"
#include "pinpoint/tracer_c.h"
#include "src/agent.h"
#include "src/noop.h"
#include "src/sql.h"
#include "test/c_api_test_helpers.h"
#include "test/it/mock_collector.h"

namespace pinpoint::test {
namespace {

using namespace std::chrono_literals;

constexpr auto kWaitTimeout = std::chrono::seconds(10);
constexpr int32_t kApplicationType = 1300;

class MapCarrier final : public HeaderReaderWriter {
public:
    std::optional<std::string_view> Get(std::string_view key) const override {
        const auto it = values_.find(std::string(key));
        if (it == values_.end()) {
            return std::nullopt;
        }
        return std::string_view(it->second);
    }

    void ForEach(std::function<bool(std::string_view, std::string_view)> callback) const override {
        for (const auto& [key, value] : values_) {
            if (!callback(key, value)) {
                return;
            }
        }
    }

    void Set(std::string_view key, std::string_view value) override {
        values_[std::string(key)] = std::string(value);
    }

private:
    std::map<std::string, std::string> values_;
};

class TestCallStack final : public CallStackReader {
public:
    void Add(std::string module, std::string function, std::string file, int line) {
        frames_.push_back({std::move(module), std::move(function), std::move(file), line});
    }

    void ForEach(std::function<void(std::string_view, std::string_view,
                                    std::string_view, int)> callback) const override {
        for (const auto& frame : frames_) {
            callback(frame.module, frame.function, frame.file, frame.line);
        }
    }

private:
    struct Frame {
        std::string module;
        std::string function;
        std::string file;
        int line;
    };
    std::vector<Frame> frames_;
};

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = kWaitTimeout) {
#if defined(PINPOINT_SANITIZER_TIMEOUT_SCALE)
    timeout *= PINPOINT_SANITIZER_TIMEOUT_SCALE;
#endif
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

std::vector<v1::PSpanMessage> all_span_messages(const CollectorSnapshot& snapshot) {
    std::vector<v1::PSpanMessage> result;
    result.reserve(snapshot.span_messages.size() + snapshot.span_batches.size() * 2);
    for (const auto& received : snapshot.span_messages) {
        result.push_back(received.message);
    }
    for (const auto& received : snapshot.span_batches) {
        for (const auto& message : received.message.span()) {
            result.push_back(message);
        }
    }
    return result;
}

size_t count_spans_by_rpc(const CollectorSnapshot& snapshot,
                          std::string_view rpc) {
    const auto messages = all_span_messages(snapshot);
    return static_cast<size_t>(std::count_if(
        messages.begin(), messages.end(),
        [rpc](const auto& message) {
            return message.has_span() && message.span().has_acceptevent() &&
                   message.span().acceptevent().rpc() == rpc;
        }));
}

size_t agent_stat_count(const CollectorSnapshot& snapshot) {
    size_t count = 0;
    for (const auto& received : snapshot.stats) {
        if (received.message.has_agentstatbatch()) {
            count += static_cast<size_t>(
                received.message.agentstatbatch().agentstat_size());
        }
    }
    return count;
}

struct TransactionTotals {
    int64_t sampled_new{0};
    int64_t sampled_continuation{0};
    int64_t unsampled_new{0};
    int64_t unsampled_continuation{0};
    int64_t skipped_new{0};
    int64_t skipped_continuation{0};
};

TransactionTotals transaction_totals_after(const CollectorSnapshot& snapshot,
                                            size_t skip) {
    TransactionTotals totals;
    size_t index = 0;
    for (const auto& received : snapshot.stats) {
        if (!received.message.has_agentstatbatch()) {
            continue;
        }
        for (const auto& stat : received.message.agentstatbatch().agentstat()) {
            if (index++ < skip || !stat.has_transaction()) {
                continue;
            }
            const auto& transaction = stat.transaction();
            totals.sampled_new += transaction.samplednewcount();
            totals.sampled_continuation += transaction.sampledcontinuationcount();
            totals.unsampled_new += transaction.unsamplednewcount();
            totals.unsampled_continuation += transaction.unsampledcontinuationcount();
            totals.skipped_new += transaction.skippednewcount();
            totals.skipped_continuation += transaction.skippedcontinuationcount();
        }
    }
    return totals;
}

int64_t max_response_time_after(const CollectorSnapshot& snapshot, size_t skip) {
    int64_t result = 0;
    size_t index = 0;
    for (const auto& received : snapshot.stats) {
        if (!received.message.has_agentstatbatch()) {
            continue;
        }
        for (const auto& stat : received.message.agentstatbatch().agentstat()) {
            if (index++ >= skip && stat.has_responsetime()) {
                result = std::max(result, stat.responsetime().max());
            }
        }
    }
    return result;
}

struct UriStatTotals {
    int64_t total_elapsed{0};
    int64_t failed_elapsed{0};
    int64_t max_elapsed{0};
    int64_t failed_max_elapsed{0};
    int64_t total_count{0};
    int64_t failed_count{0};
    size_t entries{0};
};

UriStatTotals uri_stat_totals(const CollectorSnapshot& snapshot,
                              std::string_view uri) {
    UriStatTotals totals;
    for (const auto& received : snapshot.stats) {
        if (!received.message.has_agenturistat()) {
            continue;
        }
        for (const auto& stat : received.message.agenturistat().eachuristat()) {
            if (stat.uri() != uri) {
                continue;
            }
            ++totals.entries;
            totals.total_elapsed += stat.totalhistogram().total();
            totals.failed_elapsed += stat.failedhistogram().total();
            totals.max_elapsed = std::max(totals.max_elapsed,
                                          stat.totalhistogram().max());
            totals.failed_max_elapsed = std::max(
                totals.failed_max_elapsed, stat.failedhistogram().max());
            totals.total_count += std::accumulate(
                stat.totalhistogram().histogram().begin(),
                stat.totalhistogram().histogram().end(), int64_t{0});
            totals.failed_count += std::accumulate(
                stat.failedhistogram().histogram().begin(),
                stat.failedhistogram().histogram().end(), int64_t{0});
        }
    }
    return totals;
}

std::optional<v1::PSpan> find_span_by_rpc(const CollectorSnapshot& snapshot,
                                          std::string_view rpc) {
    for (const auto& message : all_span_messages(snapshot)) {
        if (message.has_span() && message.span().has_acceptevent() &&
            message.span().acceptevent().rpc() == rpc) {
            return message.span();
        }
    }
    return std::nullopt;
}

std::vector<v1::PSpanChunk> async_chunks_for(const CollectorSnapshot& snapshot,
                                             int64_t span_id) {
    std::vector<v1::PSpanChunk> result;
    for (const auto& message : all_span_messages(snapshot)) {
        if (message.has_spanchunk() && message.spanchunk().spanid() == span_id &&
            message.spanchunk().has_localasyncid()) {
            result.push_back(message.spanchunk());
        }
    }
    return result;
}

std::vector<v1::PSpanEvent> events_for_span(const CollectorSnapshot& snapshot,
                                            int64_t span_id) {
    std::vector<v1::PSpanEvent> result;
    for (const auto& message : all_span_messages(snapshot)) {
        if (message.has_span() && message.span().spanid() == span_id) {
            result.insert(result.end(), message.span().spanevent().begin(),
                          message.span().spanevent().end());
        }
        if (message.has_spanchunk() && message.spanchunk().spanid() == span_id) {
            result.insert(result.end(), message.spanchunk().spanevent().begin(),
                          message.spanchunk().spanevent().end());
        }
    }
    return result;
}

template <typename Annotations>
const v1::PAnnotation* find_annotation(const Annotations& annotations, int32_t key) {
    const auto it = std::find_if(annotations.begin(), annotations.end(), [key](const auto& annotation) {
        return annotation.key() == key;
    });
    return it == annotations.end() ? nullptr : &*it;
}

template <typename Annotations>
bool has_string_pair_annotation(const Annotations& annotations, int32_t key,
                                std::string_view first,
                                std::string_view second) {
    return std::any_of(annotations.begin(), annotations.end(),
        [key, first, second](const auto& annotation) {
            if (annotation.key() != key) {
                return false;
            }
            const auto& pair = annotation.value().stringstringvalue();
            return pair.stringvalue1().value() == first &&
                   pair.stringvalue2().value() == second;
        });
}

bool has_uri_stat(const CollectorSnapshot& snapshot, std::string_view uri) {
    for (const auto& received : snapshot.stats) {
        if (!received.message.has_agenturistat()) {
            continue;
        }
        for (const auto& stat : received.message.agenturistat().eachuristat()) {
            if (stat.uri() == uri) {
                return true;
            }
        }
    }
    return false;
}

// Agent stats flush on a fixed tick, so batches whose interval closed before a
// sampled transaction started carry a zero count. Callers that want to inspect
// the transaction counts must pick the batch that actually carries them rather
// than whichever batch arrived first.
const v1::PAgentStat* sampled_new_agent_stat(const CollectorSnapshot& snapshot) {
    for (const auto& received : snapshot.stats) {
        if (!received.message.has_agentstatbatch()) {
            continue;
        }
        for (const auto& stat : received.message.agentstatbatch().agentstat()) {
            if (stat.has_transaction() && stat.transaction().samplednewcount() >= 1) {
                return &stat;
            }
        }
    }
    return nullptr;
}

std::vector<RpcResult> results_for(const CollectorSnapshot& snapshot,
                                   CollectorRpc rpc) {
    std::vector<RpcResult> results;
    std::copy_if(snapshot.rpc_results.begin(), snapshot.rpc_results.end(),
                 std::back_inserter(results), [rpc](const auto& result) {
                     return result.rpc == rpc;
                 });
    return results;
}

// Connections the recorded calls arrived on: the client's ephemeral port in
// grpc::ServerContext::peer() changes with every new channel, so more than
// one distinct peer proves a channel rotation happened.
size_t distinct_peers(const std::vector<RpcMetadata>& streams) {
    std::set<std::string> peers;
    for (const auto& metadata : streams) {
        peers.insert(metadata.peer);
    }
    return peers.size();
}

template <typename Message>
size_t distinct_peers(const std::vector<Received<Message>>& received) {
    std::set<std::string> peers;
    for (const auto& record : received) {
        peers.insert(record.metadata.peer);
    }
    return peers.size();
}

bool has_result(const CollectorSnapshot& snapshot,
                CollectorRpc rpc,
                grpc::StatusCode code,
                std::optional<bool> response_success = std::nullopt) {
    return std::any_of(snapshot.rpc_results.begin(), snapshot.rpc_results.end(),
        [rpc, code, response_success](const auto& result) {
            return result.rpc == rpc && result.status_code == code &&
                   (!response_success.has_value() ||
                    result.response_success == *response_success);
        });
}

size_t count_active_thread_responses(
    const CollectorSnapshot& snapshot, int32_t response_id,
    std::optional<int32_t> sequence_id = std::nullopt) {
    return static_cast<size_t>(std::count_if(
        snapshot.active_thread_count_responses.begin(),
        snapshot.active_thread_count_responses.end(),
        [response_id, sequence_id](const auto& response) {
            const auto& common = response.message.commonstreamresponse();
            return common.responseid() == response_id &&
                   (!sequence_id.has_value() ||
                    common.sequenceid() == *sequence_id);
        }));
}

bool has_api_metadata(const CollectorSnapshot& snapshot,
                      std::string_view api_info, int32_t type) {
    return std::any_of(snapshot.api_metadata.begin(),
                       snapshot.api_metadata.end(),
        [api_info, type](const auto& received) {
            return std::string_view(received.message.apiinfo()) == api_info &&
                   received.message.type() == type;
        });
}

/**
 * The host application's "business logic": a fake request handler that must
 * produce its result no matter what state the agent or the collector is in.
 * The span API is exercised on the way, like instrumented application code.
 */
int handle_instrumented_request(Agent& agent, std::string_view rpc, int input) {
    auto span = agent.NewSpan("app.request", rpc);
    auto* event = span->NewSpanEvent("app.compute");
    const int result = input * 2 + 1;
    event->EndEvent();
    span->SetStatusCode(200);
    span->EndSpan();
    return result;
}

/**
 * Asserts @p span is the shared noop singleton the agent hands out whenever
 * tracing is impossible: nothing is recorded, no identifiers are minted, and
 * outbound context injection stays empty so downstream services see an
 * untraced call.
 */
void expect_noop_span(const SpanPtr& span) {
    ASSERT_NE(span, nullptr);
    EXPECT_EQ(span, noopSpan());
    EXPECT_FALSE(span->IsSampled());
    EXPECT_TRUE(span->GetTraceId().empty());
    EXPECT_EQ(span->GetSpanId(), 0);

    auto* event = span->NewSpanEvent("noop.probe");
    ASSERT_NE(event, nullptr);
    MapCarrier outbound;
    event->InjectContext(outbound);
    EXPECT_FALSE(outbound.Get(HEADER_TRACE_ID).has_value());
    EXPECT_FALSE(outbound.Get(HEADER_SAMPLED).has_value());
    event->EndEvent();
    span->EndSpan();
}

void expect_common_metadata(const RpcMetadata& metadata, bool expect_socket_id,
                            const std::string& expected_agent_id) {
    EXPECT_EQ(metadata.value("applicationname").value_or(""), "cpp-agent-it");
    // The agent id is always auto-generated; callers pass the running
    // agent's id so the wire metadata is checked against it.
    EXPECT_EQ(expected_agent_id.size(), 22u);
    EXPECT_EQ(metadata.value("agentid").value_or(""), expected_agent_id);
    EXPECT_EQ(metadata.value("agentname").value_or(""), "cpp-it-agent-name");
    EXPECT_EQ(metadata.value("servicetype").value_or(""), std::to_string(kApplicationType));
    EXPECT_EQ(metadata.value("protocol.version").value_or(""), "100");
    EXPECT_FALSE(metadata.value("starttime").value_or("").empty());
    EXPECT_EQ(metadata.value("socketid").has_value(), expect_socket_id);
}

class AgentIntegrationTest : public ::testing::Test {
protected:
    // Agent configuration knobs consumed by config(). Tests that need
    // non-default values assign cfg_ fields before calling StartStack().
    struct Cfg {
        std::string_view sampling_type{"COUNTER"};
        std::string_view uid_version{"v3"};
        std::string_view service_name;
        std::string_view api_key;
        int sampling_counter_rate{1};
        double sampling_percent_rate{100.0};
        int sampling_new_throughput{0};
        int sampling_continue_throughput{0};
        bool url_stat_enable_trim_path{false};
        int url_stat_trim_path_depth{3};
        int url_stat_limit{1024};
        int max_event_depth{16};
        int max_event_sequence{128};
        int span_queue_size{128};
        int span_batch_max_concurrent_requests{2};
        bool enable_sql_stats{true};
        bool trace_bind_value{true};
        bool stat_enable{true};
        int max_bind_args_size{2048};
        // Production default is minutes. Left long enough that the periodic
        // re-sender never fires mid-test unless a test shortens it.
        int agent_info_refresh_interval_ms{60000};
        // Connection renewal, off (0) like production; the renewal tests
        // set both to a few hundred milliseconds.
        int grpc_channel_max_age_ms{0};
        int grpc_stream_max_age_ms{0};
        std::string_view server_record_request_headers{"[x-request-id]"};
        std::string_view server_exclude_urls{"[/excluded/**]"};
    };

    void StartCollector() {
        ASSERT_TRUE(collector_.Start());
        ASSERT_GT(collector_.agent_port(), 0);
        ASSERT_GT(collector_.span_port(), 0);
        ASSERT_GT(collector_.stat_port(), 0);
    }

    // Starts the collector and an agent built from cfg_, then blocks until
    // the agent is registered and enabled.
    void StartStack() {
        ASSERT_NO_FATAL_FAILURE(StartCollector());

        // Collector-side fault injection must be armed before the agent
        // starts (StartAgent() begins registration immediately).
        ConfigureBeforeAgentStart();

        ASSERT_NO_FATAL_FAILURE(StartTestAgent());
        ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
            return !snapshot.agent_infos.empty();
        }, kWaitTimeout));
        ASSERT_TRUE(wait_until([this] { return agent_->Enable(); }));
    }

    AgentOptions agent_options() const {
        AgentOptions options;
        // A test-only prefix prevents a developer's PINPOINT_CPP_* environment
        // from overriding the deterministic inline configuration below.
        options.env_prefix = "PINPOINT_CPP_AGENT_IT_ISOLATED";
        options.config_yaml = config();
        options.app_type = kApplicationType;
        options.server_info = "mock-collector integration server";
        options.args = {"--integration-test", "--ephemeral-ports"};
        options.libs = {"libintegration.so", "libmock-collector.so"};
        return options;
    }

    void StartTestAgent() {
        ASSERT_TRUE(StartAgent(agent_options())) << "starting the agent unexpectedly failed";
        agent_ = GlobalAgent();
        impl_ = std::dynamic_pointer_cast<AgentImpl>(agent_);
        ASSERT_NE(impl_, nullptr) << "configuration unexpectedly produced a noop agent";
    }

    void TearDown() override {
        if (agent_) {
            agent_->Shutdown();
        }
        impl_.reset();
        agent_.reset();
        collector_.Shutdown();
    }

    std::string config() const {
        std::ostringstream yaml;
        yaml
            << "Enable: true\n"
            << "ApplicationName: cpp-agent-it\n"
            << "AgentName: cpp-it-agent-name\n"
            << "UidVersion: " << cfg_.uid_version << "\n";
        if (!cfg_.service_name.empty()) {
            yaml << "ServiceName: " << cfg_.service_name << "\n";
        }
        if (!cfg_.api_key.empty()) {
            yaml << "ApiKey: " << cfg_.api_key << "\n";
        }
        yaml
            << "IsContainer: true\n"
            << "EnableCallstackTrace: true\n"
            << "Log:\n"
            // "warning", not error, and not the "warn" the level parser
            // rejects: make_config() reports a rejected setting at warning
            // level and then falls back to the default, so at error level a
            // fixture knob outside its valid range is silently ignored and
            // the test still passes, measuring something else entirely.
            << "  Level: warning\n"
            << "Collector:\n"
            << "  Host: " << collector_.host() << "\n"
            << "  AgentPort: " << collector_.agent_port() << "\n"
            << "  SpanPort: " << collector_.span_port() << "\n"
            << "  StatPort: " << collector_.stat_port() << "\n"
            << "  Grpc:\n"
            << "    ChannelMaxAgeMs: " << cfg_.grpc_channel_max_age_ms << "\n"
            << "    StreamMaxAgeMs: " << cfg_.grpc_stream_max_age_ms << "\n"
            << "  AgentInfo:\n"
            << "    RefreshIntervalMs: " << cfg_.agent_info_refresh_interval_ms << "\n"
            << "    SendRetryIntervalMs: 50\n"
            << "    MaxTryPerAttempt: 2\n"
            << "  SpanBatch:\n"
            << "    Size: 4\n"
            << "    FlushIntervalMs: 50\n"
            << "    CollectDeadlineMs: 20\n"
            << "    MaxConcurrentRequests: " << cfg_.span_batch_max_concurrent_requests << "\n"
            << "Stat:\n"
            << "  Enable: " << (cfg_.stat_enable ? "true" : "false") << "\n"
            << "  BatchCount: 1\n"
            << "  BatchInterval: 1000\n"
            << "Sampling:\n"
            << "  Type: " << cfg_.sampling_type << "\n"
            << "  CounterRate: " << cfg_.sampling_counter_rate << "\n"
            << "  PercentRate: " << cfg_.sampling_percent_rate << "\n"
            << "  NewThroughput: " << cfg_.sampling_new_throughput << "\n"
            << "  ContinueThroughput: " << cfg_.sampling_continue_throughput << "\n"
            << "Span:\n"
            << "  QueueSize: " << cfg_.span_queue_size << "\n"
            << "  MaxEventDepth: " << cfg_.max_event_depth << "\n"
            << "  MaxEventSequence: " << cfg_.max_event_sequence << "\n"
            << "  EventChunkSize: 2\n"
            << "Http:\n"
            << "  CollectUrlStat: true\n"
            << "  UrlStatEnableTrimPath: "
            << (cfg_.url_stat_enable_trim_path ? "true" : "false") << "\n"
            << "  UrlStatTrimPathDepth: " << cfg_.url_stat_trim_path_depth << "\n"
            << "  UrlStatMethodPrefix: true\n"
            << "  UrlStatLimit: " << cfg_.url_stat_limit << "\n"
            << "  Server:\n"
            << "    StatusCodeErrors: [4xx, 5xx]\n"
            << "    ExcludeUrl: " << cfg_.server_exclude_urls << "\n"
            << "    ExcludeMethod: [OPTIONS]\n"
            << "    RecordRequestHeader: " << cfg_.server_record_request_headers << "\n"
            << "    RecordRequestCookie: [session_id]\n"
            << "    RecordResponseHeader: [x-response-id]\n"
            << "  Client:\n"
            << "    RecordRequestHeader: [x-client-request]\n"
            << "    RecordRequestCookie: [client_session]\n"
            << "    RecordResponseHeader: [x-client-response]\n"
            << "Sql:\n"
            << "  EnableSqlStats: " << (cfg_.enable_sql_stats ? "true" : "false") << "\n"
            << "  TraceBindValue: "
            << (cfg_.trace_bind_value ? "true" : "false") << "\n"
            << "  MaxBindArgsSize: " << cfg_.max_bind_args_size << "\n";
        return yaml.str();
    }

    bool FlushUrlStatsUntil(std::string_view uri, int64_t expected_count) {
        const std::string expected_uri(uri);
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::this_thread::sleep_for(25ms);
            impl_->recordStats(URL_STATS);
            if (collector_.WaitFor([&expected_uri, expected_count](const auto& snapshot) {
                    return uri_stat_totals(snapshot, expected_uri).total_count >=
                           expected_count;
                }, 250ms)) {
                return true;
            }
        }
        return false;
    }

    template <size_t N>
    std::string DriveSamplingPattern(std::string_view operation,
                                     std::string_view rpc_prefix,
                                     const std::array<bool, N>& expected,
                                     MapCarrier* parent = nullptr) {
        std::string first_sampled_trace_id;
        for (size_t i = 0; i < expected.size(); ++i) {
            const auto rpc = std::string(rpc_prefix) + std::to_string(i);
            auto span = parent == nullptr
                ? agent_->NewSpan(operation, rpc)
                : agent_->NewSpan(operation, rpc, *parent);
            EXPECT_EQ(span->IsSampled(), expected[i]) << rpc;
            if (span->IsSampled() && first_sampled_trace_id.empty()) {
                first_sampled_trace_id = span->GetTraceId();
            }
            span->EndSpan();
        }
        return first_sampled_trace_id;
    }

    template <size_t N>
    void ExpectSamplingPattern(const CollectorSnapshot& snapshot,
                               std::string_view rpc_prefix,
                               const std::array<bool, N>& expected) {
        for (size_t i = 0; i < expected.size(); ++i) {
            const auto rpc = std::string(rpc_prefix) + std::to_string(i);
            EXPECT_EQ(count_spans_by_rpc(snapshot, rpc),
                      expected[i] ? 1U : 0U) << rpc;
        }
    }

    virtual void ConfigureBeforeAgentStart() {}

    Cfg cfg_;
    MockCollector collector_;
    AgentPtr agent_;
    std::shared_ptr<AgentImpl> impl_;
};

class PingFailureIntegrationTest : public AgentIntegrationTest {
protected:
    void ConfigureBeforeAgentStart() override {
        collector_.FailNext(CollectorRpc::PingSession,
                            grpc::StatusCode::UNAVAILABLE,
                            "first ping stream disconnected",
                            1);
    }
};

class PingTimeoutIntegrationTest : public AgentIntegrationTest {
protected:
    void ConfigureBeforeAgentStart() override {
        collector_.TimeoutNext(CollectorRpc::PingSession);
    }
};

class AgentInfoRetryIntegrationTest : public AgentIntegrationTest {
protected:
    void ConfigureBeforeAgentStart() override {
        collector_.FailNext(CollectorRpc::AgentInfo,
                            grpc::StatusCode::UNAVAILABLE,
                            "first registration attempt rejected");
    }
};

class CollectorUnavailableAtStartupIntegrationTest : public AgentIntegrationTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(StartCollector());

        // Keep the configured ports but remove every listening server. This
        // models an agent process starting while the collector is completely
        // unavailable and lets each test decide whether to recover or stop.
        ASSERT_TRUE(collector_.StopEndpoint(CollectorEndpoint::Agent));
        ASSERT_TRUE(collector_.StopEndpoint(CollectorEndpoint::Span));
        ASSERT_TRUE(collector_.StopEndpoint(CollectorEndpoint::Stat));
        ASSERT_NO_FATAL_FAILURE(StartTestAgent());
    }

    void RestartCollector() {
        ASSERT_TRUE(collector_.StartEndpoint(CollectorEndpoint::Span));
        ASSERT_TRUE(collector_.StartEndpoint(CollectorEndpoint::Stat));
        ASSERT_TRUE(collector_.StartEndpoint(CollectorEndpoint::Agent));
    }
};

// Connection renewal: every channel is replaced once it is 300ms (+-10%) old
// and the long-lived streams are reopened at the same age.
class ConnectionRenewalIntegrationTest : public AgentIntegrationTest {
protected:
    void SetUp() override {
        cfg_.grpc_channel_max_age_ms = 300;
        cfg_.grpc_stream_max_age_ms = 300;
    }
};

// Regression fixture: channel renewal must not depend on StreamMaxAgeMs.
class ChannelOnlyRenewalIntegrationTest : public AgentIntegrationTest {
protected:
    void SetUp() override {
        cfg_.grpc_channel_max_age_ms = 300;
        cfg_.grpc_stream_max_age_ms = 0;
    }
};

// Models an application that starts while the collector is unhealthy: the
// ports accept connections but every RPC keeps failing until EndOutage().
// Unlike CollectorUnavailableAtStartupIntegrationTest, the rejected
// registration attempts stay visible in the collector records.
class CollectorOutageAtStartupIntegrationTest : public AgentIntegrationTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(StartCollector());
        collector_.BeginOutage();
        ASSERT_NO_FATAL_FAILURE(StartTestAgent());
    }
};

TEST_F(AgentIntegrationTest, RegistersAgentAndMaintainsPingAndCommandStreams) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return !snapshot.pings.empty() && !snapshot.command_streams_v2.empty();
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    ASSERT_FALSE(snapshot.agent_infos.empty());
    const auto& received_info = snapshot.agent_infos.front();
    const auto& info = received_info.message;
    EXPECT_EQ(info.servicetype(), kApplicationType);
    EXPECT_GT(info.pid(), 0);
    EXPECT_FALSE(info.hostname().empty());
    EXPECT_FALSE(info.agentversion().empty());
    EXPECT_TRUE(info.container());
    ASSERT_TRUE(info.has_servermetadata());
    EXPECT_EQ(info.servermetadata().serverinfo(), "mock-collector integration server");
    ASSERT_EQ(info.servermetadata().vmarg_size(), 2);
    EXPECT_EQ(info.servermetadata().vmarg(0), "--integration-test");

    const auto libraries = std::find_if(
        info.servermetadata().serviceinfo().begin(),
        info.servermetadata().serviceinfo().end(),
        [](const auto& service) { return service.servicename() == "Libraries"; });
    ASSERT_NE(libraries, info.servermetadata().serviceinfo().end());
    ASSERT_EQ(libraries->servicelib_size(), 2);
    EXPECT_EQ(libraries->servicelib(1), "libmock-collector.so");

    expect_common_metadata(received_info.metadata, false, impl_->getAgentId());
    ASSERT_FALSE(snapshot.ping_streams.empty());
    expect_common_metadata(snapshot.ping_streams.front(), true, impl_->getAgentId());
    ASSERT_FALSE(snapshot.command_streams_v2.empty());
    expect_common_metadata(snapshot.command_streams_v2.front(), true, impl_->getAgentId());
    EXPECT_EQ(snapshot.command_streams_v2.front()
                  .value("supportcommandcode").value_or(""),
              "710;730");
}

TEST_F(AgentIntegrationTest, SendsV4IdentityAcrossGrpcAndTracePropagation) {
    cfg_.uid_version = "v4";
    cfg_.service_name = "cpp-it-service";
    cfg_.api_key = "cpp-it-api-key";
    ASSERT_NO_FATAL_FAILURE(StartStack());

    auto root = agent_->NewSpan("v4.server", "/v4-root");
    ASSERT_TRUE(root->IsSampled());
    const auto trace_id = root->GetTraceId();
    const auto root_span_id = root->GetSpanId();

    auto* outbound = root->NewSpanEvent("v4.client", SERVICE_TYPE_GRPC_CLIENT);
    ASSERT_NE(outbound, nullptr);
    outbound->SetDestination("v4-downstream");
    MapCarrier propagated;
    outbound->InjectContext(propagated);
    EXPECT_EQ(propagated.Get(HEADER_PARENT_APP_NAME).value_or(""),
              "cpp-agent-it");
    EXPECT_EQ(propagated.Get(HEADER_PARENT_APP_TYPE).value_or(""),
              std::to_string(kApplicationType));
    EXPECT_EQ(propagated.Get(HEADER_PARENT_SERVICE_NAME).value_or(""),
              "cpp-it-service");

    auto continued = agent_->NewSpan("v4.continued", "/v4-continued",
                                     propagated);
    ASSERT_TRUE(continued->IsSampled());
    EXPECT_EQ(continued->GetTraceId(), trace_id);
    outbound->EndEvent();
    continued->EndSpan();
    root->EndSpan();

    // Force one stat write so the v4 headers are observed on every collector
    // endpoint without waiting for the periodic collection interval.
    impl_->recordStats(AGENT_STATS);
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/v4-root").has_value() &&
               find_span_by_rpc(snapshot, "/v4-continued").has_value() &&
               !snapshot.api_metadata.empty() &&
               !snapshot.span_batches.empty() &&
               !snapshot.stats.empty() &&
               !snapshot.ping_streams.empty() &&
               !snapshot.command_streams_v2.empty();
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    ASSERT_FALSE(snapshot.agent_infos.empty());
    const auto agent_id = snapshot.agent_infos.front().metadata
                              .value("agentid").value_or("");
    const auto start_time = snapshot.agent_infos.front().metadata
                                .value("starttime").value_or("");
    ASSERT_EQ(agent_id.size(), 22U);
    EXPECT_EQ(impl_->getAgentId(), agent_id);
    ASSERT_FALSE(start_time.empty());

    const auto expect_v4_metadata = [&](const RpcMetadata& metadata,
                                        bool expect_socket_id) {
        EXPECT_EQ(metadata.value("applicationname").value_or(""),
                  "cpp-agent-it");
        EXPECT_EQ(metadata.value("agentid").value_or(""), agent_id);
        EXPECT_EQ(metadata.value("agentname").value_or(""),
                  "cpp-it-agent-name");
        EXPECT_EQ(metadata.value("starttime").value_or(""), start_time);
        EXPECT_EQ(metadata.value("servicetype").value_or(""),
                  std::to_string(kApplicationType));
        EXPECT_EQ(metadata.value("protocol.version").value_or(""), "400");
        EXPECT_EQ(metadata.value("servicename").value_or(""),
                  "cpp-it-service");
        EXPECT_EQ(metadata.value("apikey").value_or(""), "cpp-it-api-key");
        EXPECT_EQ(metadata.value("socketid").has_value(), expect_socket_id);
    };

    expect_v4_metadata(snapshot.agent_infos.front().metadata, false);
    expect_v4_metadata(snapshot.api_metadata.front().metadata, false);
    expect_v4_metadata(snapshot.span_batches.front().metadata, false);
    expect_v4_metadata(snapshot.stat_streams.front(), false);
    expect_v4_metadata(snapshot.ping_streams.front(), true);
    expect_v4_metadata(snapshot.command_streams_v2.front(), true);

    const auto root_wire = find_span_by_rpc(snapshot, "/v4-root");
    ASSERT_TRUE(root_wire.has_value());
    EXPECT_EQ(root_wire->transactionid().agentid(), agent_id);
    EXPECT_EQ(root_wire->spanid(), root_span_id);
    const auto continued_wire = find_span_by_rpc(snapshot, "/v4-continued");
    ASSERT_TRUE(continued_wire.has_value());
    ASSERT_TRUE(continued_wire->acceptevent().has_parentinfo());
    EXPECT_EQ(continued_wire->acceptevent().parentinfo()
                  .parentapplicationname(),
              "cpp-agent-it");
    EXPECT_EQ(continued_wire->acceptevent().parentinfo()
                  .parentapplicationtype(),
              kApplicationType);
    EXPECT_EQ(continued_wire->acceptevent().parentinfo()
                  .parentservicename(),
              "cpp-it-service");
    EXPECT_EQ(continued_wire->acceptevent().parentinfo().acceptorhost(),
              "v4-downstream");

    // The API key is intentionally present in gRPC metadata but must never be
    // copied into the AgentInfo payload/config summary.
    EXPECT_EQ(snapshot.agent_infos.front().message.SerializeAsString().find(
                  "cpp-it-api-key"),
              std::string::npos);
}

TEST_F(PingFailureIntegrationTest, ReconnectsPingStreamAfterResponseError) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return snapshot.ping_streams.size() >= 2 &&
               !snapshot.pings.empty() &&
               has_result(snapshot, CollectorRpc::PingSession,
                          grpc::StatusCode::UNAVAILABLE, false);
    }, kWaitTimeout));

    // Each ping stream carries a fresh socket id so the collector can tell a
    // reconnect from a duplicate registration.
    const auto snapshot = collector_.snapshot();
    ASSERT_GE(snapshot.ping_streams.size(), 2U);
    const auto first_socket = snapshot.ping_streams[0].value("socketid").value_or("");
    const auto second_socket = snapshot.ping_streams[1].value("socketid").value_or("");
    ASSERT_FALSE(first_socket.empty());
    ASSERT_FALSE(second_socket.empty());
    EXPECT_EQ(std::stoll(second_socket), std::stoll(first_socket) + 1);
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(PingTimeoutIntegrationTest, RecyclesPingStreamWhenCollectorNeverResponds) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return snapshot.ping_streams.size() >= 2 &&
               has_result(snapshot, CollectorRpc::PingSession,
                          grpc::StatusCode::DEADLINE_EXCEEDED, false);
    }, std::chrono::seconds(15)));
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(AgentIntegrationTest, SendsAllMetadataAndCompleteSpanShapes) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    auto root = agent_->NewSpan("http.server", "/orders/42");
    ASSERT_TRUE(root->IsSampled());
    const auto root_trace_id = root->GetTraceId();
    const auto root_span_id = root->GetSpanId();

    root->SetServiceType(SERVICE_TYPE_CPP);
    root->SetRemoteAddress("192.0.2.10");
    root->SetEndPoint("orders.internal:8443");
    root->SetAcceptorHost("api.example.test");
    root->SetStatusCode(503);
    root->SetError("RootFailure", "upstream unavailable");
    root->SetUrlStat("/orders/{id}", "GET", 503);

    const SqlUid annotation_uid{0, 1, 2, 3, 4, 5, 6, 7,
                                8, 9, 10, 11, 12, 13, 14, 15};
    root->SetAnnotation(9000, 7);
    root->SetAnnotation(9001, INT64_C(9000000000));
    root->SetAnnotation(9002, "root-value");
    root->SetAnnotation(9003, "left", "right");
    // The remaining collector-side formats are internal-only now; record them
    // straight into the span's annotation container to keep their wire shapes
    // covered end to end.
    auto* root_impl = dynamic_cast<SpanImpl*>(root.get());
    ASSERT_NE(root_impl, nullptr);
    auto* root_annotations = root_impl->getSpanData()->getAnnotations();
    root_annotations->AppendData(9004, AnnotationData(11, std::make_shared<const std::string>("one"), "two"));
    root_annotations->AppendData(9005, AnnotationData(annotation_uid, std::make_shared<const std::string>("sql"), "args"));
    root_annotations->AppendLongIntIntByteByteString(
        9006, 123456, 1, 2, 3, 4, "network-detail");

    MapCarrier server_headers;
    server_headers.Set("x-request-id", "request-123");
    root->RecordHeader(HTTP_REQUEST, server_headers);

    MapCarrier logging;
    root->SetLogging(logging);
    EXPECT_EQ(logging.Get("PtxId").value_or(""), root_trace_id);
    EXPECT_EQ(logging.Get("PspanId").value_or(""), std::to_string(root_span_id));

    auto* outbound = root->NewSpanEvent("database.query", SERVICE_TYPE_MYSQL_QUERY);
    ASSERT_NE(outbound, nullptr);
    outbound->SetDestination("mysql-primary");
    outbound->SetEndPoint("db.example.test:3306");
    outbound->SetSqlQuery("SELECT * FROM orders WHERE id = ?", {42});

    MapCarrier client_headers;
    client_headers.Set("x-client-request", "client-request-456");
    outbound->RecordHeader(HTTP_REQUEST, client_headers);

    TestCallStack call_stack;
    call_stack.Add("orders", "load_order", "orders.cpp", 73);
    call_stack.Add("database", "execute", "database.cpp", 118);
    outbound->SetError("DatabaseError", "connection refused", call_stack);

    auto async = root->NewAsyncSpan("async.order.audit");
    ASSERT_TRUE(async->IsSampled());
    auto* async_event = async->GetSpanEvent();
    ASSERT_NE(async_event, nullptr);
    async_event->SetOperationName("async.audit.worker");
    async_event->SetDestination("audit-queue");

    MapCarrier propagated;
    outbound->InjectContext(propagated);
    EXPECT_EQ(propagated.Get(HEADER_TRACE_ID).value_or(""), root_trace_id);
    EXPECT_EQ(propagated.Get(HEADER_PARENT_SPAN_ID).value_or(""),
              std::to_string(root_span_id));
    EXPECT_EQ(propagated.Get(HEADER_HOST).value_or(""), "mysql-primary");
    ASSERT_TRUE(propagated.Get(HEADER_SPAN_ID).has_value());
    const auto propagated_span_id = std::stoll(
        std::string(propagated.Get(HEADER_SPAN_ID).value()));

    auto continued = agent_->NewSpan("continued.server", "/downstream", propagated);
    ASSERT_TRUE(continued->IsSampled());
    EXPECT_EQ(continued->GetTraceId(), root_trace_id);
    EXPECT_EQ(continued->GetSpanId(), propagated_span_id);
    continued->SetStatusCode(200);

    outbound->EndEvent();
    async->EndSpan();
    continued->EndSpan();

    // Cross the event-chunk threshold so both PSpanChunk and final PSpan wire
    // shapes are exercised in addition to the async PSpanChunk above.
    for (int i = 0; i < 3; ++i) {
        auto* event = root->NewSpanEvent("chunk.event." + std::to_string(i));
        ASSERT_NE(event, nullptr);
        event->SetAnnotation(9100 + i, i);
        event->EndEvent();
    }
    root->EndSpan();

    MapCarrier unsampled_context;
    unsampled_context.Set(HEADER_SAMPLED, "s0");
    auto unsampled = agent_->NewSpan("not.sampled", "/unsampled", unsampled_context);
    EXPECT_FALSE(unsampled->IsSampled());
    unsampled->EndSpan();
    EXPECT_FALSE(agent_->NewSpan("excluded", "/excluded/path")->IsSampled());
    MapCarrier no_parent;
    EXPECT_FALSE(agent_->NewSpan("excluded.method", "/method", "OPTIONS", no_parent)->IsSampled());

    ASSERT_TRUE(collector_.WaitFor([root_span_id](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/orders/42").has_value() &&
               find_span_by_rpc(snapshot, "/downstream").has_value() &&
               !async_chunks_for(snapshot, root_span_id).empty() &&
               !snapshot.api_metadata.empty() &&
               !snapshot.string_metadata.empty() &&
               !snapshot.sql_uid_metadata.empty() &&
               !snapshot.exception_metadata.empty();
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto root_wire = find_span_by_rpc(snapshot, "/orders/42");
    ASSERT_TRUE(root_wire.has_value());
    EXPECT_EQ(root_wire->spanid(), root_span_id);
    EXPECT_EQ(root_wire->transactionid().agentid(), impl_->getAgentId());
    EXPECT_EQ(root_wire->acceptevent().remoteaddr(), "192.0.2.10");
    EXPECT_EQ(root_wire->acceptevent().endpoint(), "orders.internal:8443");
    EXPECT_EQ(root_wire->err(), 1);
    EXPECT_NE(root_wire->loggingtransactioninfo(), 0);
    ASSERT_TRUE(root_wire->has_exceptioninfo());
    EXPECT_EQ(root_wire->exceptioninfo().stringvalue().value(), "upstream unavailable");

    const auto* int_annotation = find_annotation(root_wire->annotation(), 9000);
    ASSERT_NE(int_annotation, nullptr);
    EXPECT_EQ(int_annotation->value().intvalue(), 7);
    const auto* long_annotation = find_annotation(root_wire->annotation(), 9001);
    ASSERT_NE(long_annotation, nullptr);
    EXPECT_EQ(long_annotation->value().longvalue(), INT64_C(9000000000));
    const auto* string_annotation = find_annotation(root_wire->annotation(), 9002);
    ASSERT_NE(string_annotation, nullptr);
    EXPECT_EQ(string_annotation->value().stringvalue(), "root-value");
    const auto* pair_annotation = find_annotation(root_wire->annotation(), 9003);
    ASSERT_NE(pair_annotation, nullptr);
    EXPECT_EQ(pair_annotation->value().stringstringvalue().stringvalue2().value(), "right");
    const auto* uid_annotation = find_annotation(root_wire->annotation(), 9005);
    ASSERT_NE(uid_annotation, nullptr);
    EXPECT_EQ(uid_annotation->value().bytesstringstringvalue().bytesvalue().size(), 16U);
    const auto* network_annotation = find_annotation(root_wire->annotation(), 9006);
    ASSERT_NE(network_annotation, nullptr);
    EXPECT_EQ(network_annotation->value()
                  .longintintbytebytestringvalue().stringvalue().value(),
              "network-detail");
    const auto* status_annotation = find_annotation(
        root_wire->annotation(), ANNOTATION_HTTP_STATUS_CODE);
    ASSERT_NE(status_annotation, nullptr);
    EXPECT_EQ(status_annotation->value().intvalue(), 503);
    const auto* header_annotation = find_annotation(
        root_wire->annotation(), ANNOTATION_HTTP_REQUEST_HEADER);
    ASSERT_NE(header_annotation, nullptr);
    EXPECT_EQ(header_annotation->value().stringstringvalue().stringvalue2().value(),
              "request-123");

    const auto continued_wire = find_span_by_rpc(snapshot, "/downstream");
    ASSERT_TRUE(continued_wire.has_value());
    EXPECT_EQ(continued_wire->transactionid().agentid(),
              root_wire->transactionid().agentid());
    EXPECT_EQ(continued_wire->transactionid().agentstarttime(),
              root_wire->transactionid().agentstarttime());
    EXPECT_EQ(continued_wire->transactionid().sequence(),
              root_wire->transactionid().sequence());
    EXPECT_EQ(continued_wire->parentspanid(), root_span_id);

    const auto events = events_for_span(snapshot, root_span_id);
    const auto database_event = std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.servicetype() == SERVICE_TYPE_MYSQL_QUERY;
    });
    ASSERT_NE(database_event, events.end());
    EXPECT_NE(find_annotation(database_event->annotation(), ANNOTATION_SQL_UID), nullptr);
    EXPECT_NE(find_annotation(database_event->annotation(), ANNOTATION_EXCEPTION_ID), nullptr);
    EXPECT_NE(find_annotation(database_event->annotation(),
                              ANNOTATION_HTTP_REQUEST_HEADER), nullptr);

    ASSERT_FALSE(snapshot.sql_uid_metadata.empty());
    EXPECT_EQ(snapshot.sql_uid_metadata.front().message.sqluid().size(), 16U);
    ASSERT_FALSE(snapshot.exception_metadata.empty());
    const auto& exception = snapshot.exception_metadata.front().message;
    EXPECT_EQ(exception.spanid(), root_span_id);
    EXPECT_EQ(exception.uritemplate(), "/orders/{id}");
    ASSERT_EQ(exception.exceptions_size(), 1);
    EXPECT_EQ(exception.exceptions(0).exceptionclassname(), "DatabaseError")
        << "The class name is the SetError name, not the top frame's module";
    EXPECT_EQ(exception.exceptions(0).exceptiondepth(), 0);
    EXPECT_EQ(exception.exceptions(0).exceptionmessage(), "connection refused");
    ASSERT_EQ(exception.exceptions(0).stacktraceelement_size(), 2);
    EXPECT_EQ(exception.exceptions(0).stacktraceelement(1).methodname(), "execute");

    ASSERT_FALSE(snapshot.span_batches.empty());
    expect_common_metadata(snapshot.span_batches.front().metadata, false, impl_->getAgentId());
}

TEST_F(AgentIntegrationTest, StreamsAgentAndUrlStatistics) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    auto active = agent_->NewSpan("active.request", "/active");
    ASSERT_TRUE(active->IsSampled());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        for (const auto& received : snapshot.stats) {
            if (!received.message.has_agentstatbatch()) {
                continue;
            }
            for (const auto& stat : received.message.agentstatbatch().agentstat()) {
                if (!stat.has_activetrace() || !stat.activetrace().has_histogram()) {
                    continue;
                }
                const auto& counts = stat.activetrace().histogram().activetracecount();
                if (std::accumulate(counts.begin(), counts.end(), int64_t{0}) > 0) {
                    return true;
                }
            }
        }
        return false;
    }, kWaitTimeout));

    active->EndSpan();
    auto url_span = agent_->NewSpan("url.stat.request", "/orders/99");
    ASSERT_TRUE(url_span->IsSampled());
    url_span->SetUrlStat("/orders/{id}", "GET", 500);
    url_span->SetStatusCode(500);
    url_span->EndSpan();

    // URL stats normally flush every 30 seconds. Trigger the existing sender
    // through AgentService after the add worker has consumed the span; retrying
    // also makes the add-vs-take snapshot handoff deterministic.
    bool saw_url_stat = false;
    for (int attempt = 0; attempt < 20 && !saw_url_stat; ++attempt) {
        std::this_thread::sleep_for(25ms);
        impl_->recordStats(URL_STATS);
        saw_url_stat = collector_.WaitFor([](const auto& snapshot) {
            return has_uri_stat(snapshot, "GET /orders/{id}");
        }, 250ms);
    }
    ASSERT_TRUE(saw_url_stat);

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return sampled_new_agent_stat(snapshot) != nullptr;
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    ASSERT_FALSE(snapshot.stat_streams.empty());
    expect_common_metadata(snapshot.stat_streams.front(), false, impl_->getAgentId());

    const auto* agent_stat = sampled_new_agent_stat(snapshot);
    ASSERT_NE(agent_stat, nullptr);
    EXPECT_GT(agent_stat->timestamp(), 0);
    // The measured period since the previous collection, not BatchInterval
    // (1000ms) exactly — only the first row of a run reports the setting, and
    // this is whichever row carried the sampled transaction. The exact value
    // is pinned by test_stat's CollectIntervalIsMeasured... test.
    EXPECT_GE(agent_stat->collectinterval(), 500);
    EXPECT_LT(agent_stat->collectinterval(), 10000);
    ASSERT_TRUE(agent_stat->has_responsetime());
    ASSERT_TRUE(agent_stat->has_totalthread());
    EXPECT_GT(agent_stat->totalthread().totalthreadcount(), 0);

    bool checked_url_stat = false;
    for (const auto& received : snapshot.stats) {
        if (!received.message.has_agenturistat()) {
            continue;
        }
        for (const auto& stat : received.message.agenturistat().eachuristat()) {
            if (stat.uri() != "GET /orders/{id}") {
                continue;
            }
            EXPECT_EQ(received.message.agenturistat().bucketversion(), 0);
            ASSERT_TRUE(stat.has_totalhistogram());
            ASSERT_TRUE(stat.has_failedhistogram());
            EXPECT_EQ(std::accumulate(stat.totalhistogram().histogram().begin(),
                                      stat.totalhistogram().histogram().end(), 0),
                      1);
            EXPECT_EQ(std::accumulate(stat.failedhistogram().histogram().begin(),
                                      stat.failedhistogram().histogram().end(), 0),
                      1);
            checked_url_stat = true;
        }
    }
    EXPECT_TRUE(checked_url_stat);
}

TEST_F(AgentIntegrationTest, FinalizesScopedAndOpenSpanEventsExactlyOnce) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    auto span = agent_->NewSpan("span.lifecycle", "/span-lifecycle");
    ASSERT_TRUE(span->IsSampled());
    const auto span_id = span->GetSpanId();

    {
        helper::ScopedSpanEvent scoped(span, "scoped.event",
                                       SERVICE_TYPE_CPP_FUNC);
        ASSERT_NE(scoped.value(), nullptr);
        scoped->SetDestination("scoped-worker");
    }

    auto* open_event = span->NewSpanEvent("implicitly.finished.event",
                                          SERVICE_TYPE_REDIS);
    ASSERT_NE(open_event, nullptr);
    open_event->SetDestination("redis-cache");

    // EndSpan owns the final cleanup for forgotten events, and a duplicate
    // call must not enqueue another final PSpan.
    span->EndSpan();
    span->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([span_id](const auto& snapshot) {
        return count_spans_by_rpc(snapshot, "/span-lifecycle") == 1 &&
               events_for_span(snapshot, span_id).size() == 2;
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    EXPECT_EQ(count_spans_by_rpc(snapshot, "/span-lifecycle"), 1U);
    const auto events = events_for_span(snapshot, span_id);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const auto& event) {
                  return event.servicetype() == SERVICE_TYPE_CPP_FUNC;
              }), 1);
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const auto& event) {
                  return event.servicetype() == SERVICE_TYPE_REDIS;
              }), 1);
}

TEST_F(AgentIntegrationTest,
       PreservesOutOfOrderEventsAndIgnoresPostFinishMutations) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    auto span = agent_->NewSpan("event.lifecycle", "/event-lifecycle");
    ASSERT_TRUE(span->IsSampled());
    const auto span_id = span->GetSpanId();

    auto* outer = span->NewSpanEvent("event.outer", SERVICE_TYPE_REDIS);
    ASSERT_NE(outer, nullptr);
    outer->SetDestination("outer-before");

    // An empty constructor operation leaves apiId unset, making the operation
    // itself visible as ANNOTATION_API on the wire.
    auto* inner = span->NewSpanEvent("", SERVICE_TYPE_MEMCACHED);
    ASSERT_NE(inner, nullptr);
    inner->SetOperationName("event.inner.before");
    inner->SetDestination("inner-before");
    inner->SetEndPoint("inner-before.example.test:11211");
    inner->SetError("BeforeError", "before-error-message");
    inner->SetAnnotation(9200, "before-annotation");

    // Ending the outer event first implicitly unwinds the inner event. The two
    // completed events also cross EventChunkSize=2 and are handed to the gRPC
    // worker, so every later mutation must be a safe no-op.
    outer->EndEvent();
    inner->SetServiceType(SERVICE_TYPE_KAFKA);
    inner->SetOperationName("event.inner.after");
    inner->SetStartTime(std::chrono::system_clock::now() + 1h);
    inner->SetDestination("inner-after");
    inner->SetEndPoint("inner-after.example.test:9092");
    inner->SetError("AfterError", "after-error-message");
    inner->SetSqlQuery("SELECT * FROM post_finish_guard WHERE id = 7",
                       {std::string_view("sensitive-after-finish")});
    MapCarrier post_finish_headers;
    post_finish_headers.Set("x-client-request", "after-finish-header");
    inner->RecordHeader(HTTP_REQUEST, post_finish_headers);
    inner->SetAnnotation(9201, "after-annotation");

    auto* later = span->NewSpanEvent("event.later", SERVICE_TYPE_KAFKA);
    ASSERT_NE(later, nullptr);
    // A duplicate EndEvent on the implicitly finished inner event must not pop
    // the newly active event from the span stack.
    inner->EndEvent();
    EXPECT_EQ(span->GetSpanEvent(), later);
    later->EndEvent();
    outer->EndEvent();
    span->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([span_id](const auto& snapshot) {
        return count_spans_by_rpc(snapshot, "/event-lifecycle") == 1 &&
               events_for_span(snapshot, span_id).size() == 3;
    }, kWaitTimeout));

    auto events = events_for_span(collector_.snapshot(), span_id);
    ASSERT_EQ(events.size(), 3U);
    std::sort(events.begin(), events.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.sequence() < rhs.sequence();
    });
    EXPECT_EQ(events[0].sequence(), 0);
    EXPECT_EQ(events[0].depth(), 1);
    EXPECT_EQ(events[0].servicetype(), SERVICE_TYPE_REDIS);
    EXPECT_EQ(events[1].sequence(), 1);
    EXPECT_EQ(events[1].depth(), 2);
    EXPECT_EQ(events[1].servicetype(), SERVICE_TYPE_MEMCACHED);
    EXPECT_EQ(events[2].sequence(), 2);
    EXPECT_EQ(events[2].servicetype(), SERVICE_TYPE_KAFKA);

    const auto& inner_wire = events[1];
    const auto* operation = find_annotation(inner_wire.annotation(),
                                            ANNOTATION_API);
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->value().stringvalue(), "event.inner.before");
    ASSERT_TRUE(inner_wire.has_nextevent());
    EXPECT_EQ(inner_wire.nextevent().messageevent().destinationid(),
              "inner-before");
    EXPECT_EQ(inner_wire.nextevent().messageevent().endpoint(),
              "inner-before.example.test:11211");
    ASSERT_TRUE(inner_wire.has_exceptioninfo());
    EXPECT_EQ(inner_wire.exceptioninfo().stringvalue().value(),
              "before-error-message");

    const auto* before_annotation = find_annotation(inner_wire.annotation(), 9200);
    ASSERT_NE(before_annotation, nullptr);
    EXPECT_EQ(before_annotation->value().stringvalue(), "before-annotation");
    EXPECT_EQ(find_annotation(inner_wire.annotation(), 9201), nullptr);
    EXPECT_EQ(find_annotation(inner_wire.annotation(), ANNOTATION_SQL_UID), nullptr);
    EXPECT_EQ(find_annotation(inner_wire.annotation(),
                              ANNOTATION_HTTP_REQUEST_HEADER), nullptr);
}

TEST_F(AgentIntegrationTest, HttpHelpersPopulateServerAndClientWireData) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    auto span = agent_->NewSpan("http.helper.server", "/http-helper");
    ASSERT_TRUE(span->IsSampled());
    const auto span_id = span->GetSpanId();

    MapCarrier server_request;
    server_request.Set("X-Forwarded-For", "203.0.113.7, 10.0.0.1");
    server_request.Set("Pinpoint-ProxyNginx", "t=1710000000.125 D=37");
    server_request.Set("x-request-id", "server-request-1");
    MapCarrier server_cookie;
    server_cookie.Set("session_id", "server-session-2");
    helper::TraceHttpServerRequest(span, "192.0.2.20:8443",
                                   "frontend.example.test:443",
                                   server_request, server_cookie);

    auto* client = span->NewSpanEvent("http.helper.client");
    ASSERT_NE(client, nullptr);
    MapCarrier client_request;
    client_request.Set("x-client-request", "client-request-3");
    MapCarrier client_cookie;
    client_cookie.Set("client_session", "client-session-4");
    helper::TraceHttpClientRequest(client, "inventory.example.test:8443",
                                   "https://inventory.example.test/items/42",
                                   client_request, client_cookie);

    MapCarrier client_response;
    client_response.Set("x-client-response", "client-response-5");
    helper::TraceHttpClientResponse(client, 429, client_response);
    client->EndEvent();

    MapCarrier server_response;
    server_response.Set("x-response-id", "server-response-6");
    helper::TraceHttpServerResponse(span, "/http-helper/{id}", "POST",
                                    503, server_response);
    span->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([span_id](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/http-helper").has_value() &&
               !events_for_span(snapshot, span_id).empty();
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto wire = find_span_by_rpc(snapshot, "/http-helper");
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->acceptevent().remoteaddr(), "203.0.113.7");
    EXPECT_EQ(wire->acceptevent().endpoint(), "frontend.example.test:443");
    EXPECT_EQ(wire->err(), 1);

    const auto* proxy = find_annotation(wire->annotation(),
                                        ANNOTATION_HTTP_PROXY_HEADER);
    ASSERT_NE(proxy, nullptr);
    const auto& proxy_value = proxy->value().longintintbytebytestringvalue();
    EXPECT_EQ(proxy_value.longvalue(), INT64_C(1710000000125));
    EXPECT_EQ(proxy_value.intvalue1(), 2);
    EXPECT_EQ(proxy_value.intvalue2(), 37);

    const auto* server_request_header = find_annotation(
        wire->annotation(), ANNOTATION_HTTP_REQUEST_HEADER);
    ASSERT_NE(server_request_header, nullptr);
    EXPECT_EQ(server_request_header->value().stringstringvalue()
                  .stringvalue2().value(), "server-request-1");
    const auto* server_cookie_header = find_annotation(
        wire->annotation(), ANNOTATION_HTTP_COOKIE);
    ASSERT_NE(server_cookie_header, nullptr);
    EXPECT_EQ(server_cookie_header->value().stringstringvalue()
                  .stringvalue2().value(), "server-session-2");
    const auto* server_response_header = find_annotation(
        wire->annotation(), ANNOTATION_HTTP_RESPONSE_HEADER);
    ASSERT_NE(server_response_header, nullptr);
    EXPECT_EQ(server_response_header->value().stringstringvalue()
                  .stringvalue2().value(), "server-response-6");

    const auto events = events_for_span(snapshot, span_id);
    const auto client_event = std::find_if(events.begin(), events.end(),
        [](const auto& event) {
            return event.servicetype() == SERVICE_TYPE_CPP_HTTP_CLIENT;
        });
    ASSERT_NE(client_event, events.end());
    ASSERT_TRUE(client_event->has_nextevent());
    ASSERT_TRUE(client_event->nextevent().has_messageevent());
    EXPECT_EQ(client_event->nextevent().messageevent().endpoint(),
              "inventory.example.test:8443");
    EXPECT_EQ(client_event->nextevent().messageevent().destinationid(),
              "inventory.example.test:8443");

    const auto* url = find_annotation(client_event->annotation(),
                                      ANNOTATION_HTTP_URL);
    ASSERT_NE(url, nullptr);
    EXPECT_EQ(url->value().stringvalue(),
              "https://inventory.example.test/items/42");
    const auto* status = find_annotation(client_event->annotation(),
                                         ANNOTATION_HTTP_STATUS_CODE);
    ASSERT_NE(status, nullptr);
    EXPECT_EQ(status->value().intvalue(), 429);
    const auto* client_request_header = find_annotation(
        client_event->annotation(), ANNOTATION_HTTP_REQUEST_HEADER);
    ASSERT_NE(client_request_header, nullptr);
    EXPECT_EQ(client_request_header->value().stringstringvalue()
                  .stringvalue2().value(), "client-request-3");
    const auto* client_cookie_header = find_annotation(
        client_event->annotation(), ANNOTATION_HTTP_COOKIE);
    ASSERT_NE(client_cookie_header, nullptr);
    EXPECT_EQ(client_cookie_header->value().stringstringvalue()
                  .stringvalue2().value(), "client-session-4");
    const auto* client_response_header = find_annotation(
        client_event->annotation(), ANNOTATION_HTTP_RESPONSE_HEADER);
    ASSERT_NE(client_response_header, nullptr);
    EXPECT_EQ(client_response_header->value().stringstringvalue()
                  .stringvalue2().value(), "client-response-5");
}

// The uid cache stores whatever the normalizer produced, and the normalizer
// produces an empty string for all-comment SQL (src/sql.cpp:69-71 returns the
// empty result, src/span_event.cpp:400-403 does not filter it). An empty key
// is below Sql.CacheLengthLimit, so src/cache.h:792 bypasses() says false and
// the entry really is cached under "". Eviction on send failure must therefore
// key off the meta's `cached_` flag, not the key's shape: keying off emptiness
// both skipped this entry and, symmetrically, would have let a bypassed
// statement's release evict it.
TEST_F(AgentIntegrationTest, SqlUidEvictionFollowsCacheMembershipNotKeyShape) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    // The reachable input: Sql.RemoveComments defaults to true.
    ASSERT_TRUE(SqlNormalizer().normalize("/* hint */").normalized_sql.empty());

    constexpr std::string_view empty_key = "";
    const std::string long_sql = "SELECT " + std::string(70000, 'a') + " FROM t";

    const auto empty_uid = impl_->cacheSqlUid(empty_key);   // miss: row 1
    ASSERT_TRUE(empty_uid.has_value());
    EXPECT_EQ(*empty_uid, *impl_->cacheSqlUid(empty_key));  // hit: no row

    // Releasing a bypassed statement must leave the "" entry alone.
    const auto long_uid = impl_->cacheSqlUid(long_sql);
    ASSERT_TRUE(long_uid.has_value());
    impl_->removeCacheSqlUid(SqlUidMeta(*long_uid, long_sql, /*cached=*/false));
    EXPECT_EQ(*empty_uid, *impl_->cacheSqlUid(empty_key));  // still a hit

    // Releasing the "" meta does evict, so the next use re-registers: row 2.
    impl_->removeCacheSqlUid(SqlUidMeta(*empty_uid, empty_key));
    EXPECT_EQ(*empty_uid, *impl_->cacheSqlUid(empty_key));

    const auto count_empty = [](const auto& snapshot) {
        return std::count_if(snapshot.sql_uid_metadata.begin(),
                             snapshot.sql_uid_metadata.end(),
            [](const auto& received) { return received.message.sql().empty(); });
    };
    ASSERT_TRUE(collector_.WaitFor([&](const auto& snapshot) {
        return count_empty(snapshot) >= 2;
    }, kWaitTimeout));
    EXPECT_EQ(count_empty(collector_.snapshot()), 2)
        << "a third row means the bypassed release evicted the \"\" entry";
}

TEST_F(AgentIntegrationTest, CachesDeduplicateAndInvalidateCollectorMetadata) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    constexpr std::string_view api_key = "cache.shared.api";
    const auto api_default = impl_->cacheApi(api_key, API_TYPE_DEFAULT);
    const auto api_default_hit = impl_->cacheApi(api_key, API_TYPE_DEFAULT);
    const auto api_invocation = impl_->cacheApi(api_key, API_TYPE_INVOCATION);
    impl_->removeCacheApi(ApiMeta(api_default, API_TYPE_DEFAULT, api_key));
    const auto api_default_reloaded = impl_->cacheApi(api_key, API_TYPE_DEFAULT);

    constexpr std::string_view error_key = "CacheIntegrationError";
    const auto error_id = impl_->cacheError(error_key);
    const auto error_hit = impl_->cacheError(error_key);
    impl_->removeCacheError(StringMeta(error_id, error_key, STRING_META_ERROR));
    const auto error_reloaded = impl_->cacheError(error_key);

    constexpr std::string_view sql_key =
        "SELECT value FROM cache_entries WHERE cache_key=?";
    const auto sql_id = impl_->cacheSql(sql_key);
    const auto sql_hit = impl_->cacheSql(sql_key);
    impl_->removeCacheSql(StringMeta(sql_id, sql_key, STRING_META_SQL));
    const auto sql_reloaded = impl_->cacheSql(sql_key);

    constexpr std::string_view sql_uid_key =
        "SELECT value FROM cache_entries WHERE cache_key=:key";
    const auto sql_uid = impl_->cacheSqlUid(sql_uid_key);
    const auto sql_uid_hit = impl_->cacheSqlUid(sql_uid_key);
    ASSERT_TRUE(sql_uid.has_value());
    ASSERT_TRUE(sql_uid_hit.has_value());
    impl_->removeCacheSqlUid(SqlUidMeta(*sql_uid, sql_uid_key));
    const auto sql_uid_reloaded = impl_->cacheSqlUid(sql_uid_key);
    ASSERT_TRUE(sql_uid_reloaded.has_value());

    EXPECT_EQ(api_default, api_default_hit);
    EXPECT_NE(api_default, api_invocation);
    EXPECT_NE(api_default, api_default_reloaded);
    EXPECT_EQ(error_id, error_hit);
    EXPECT_NE(error_id, error_reloaded);
    EXPECT_EQ(sql_id, sql_hit);
    EXPECT_NE(sql_id, sql_reloaded);
    EXPECT_EQ(*sql_uid, *sql_uid_hit);
    EXPECT_EQ(*sql_uid, *sql_uid_reloaded);

    ASSERT_TRUE(collector_.WaitFor([&](const auto& snapshot) {
        const auto api_count = std::count_if(
            snapshot.api_metadata.begin(), snapshot.api_metadata.end(),
            [&](const auto& received) {
                return received.message.apiinfo() == api_key;
            });
        const auto error_count = std::count_if(
            snapshot.string_metadata.begin(), snapshot.string_metadata.end(),
            [&](const auto& received) {
                return received.message.stringvalue() == error_key;
            });
        const auto sql_count = std::count_if(
            snapshot.sql_metadata.begin(), snapshot.sql_metadata.end(),
            [&](const auto& received) {
                return received.message.sql() == sql_key;
            });
        const auto uid_count = std::count_if(
            snapshot.sql_uid_metadata.begin(), snapshot.sql_uid_metadata.end(),
            [&](const auto& received) {
                return received.message.sql() == sql_uid_key;
            });
        return api_count == 3 && error_count == 2 && sql_count == 2 &&
               uid_count == 2;
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    EXPECT_EQ(std::count_if(snapshot.api_metadata.begin(),
                            snapshot.api_metadata.end(),
        [&](const auto& received) {
            return received.message.apiinfo() == api_key &&
                   received.message.type() == API_TYPE_DEFAULT &&
                   (received.message.apiid() == api_default ||
                    received.message.apiid() == api_default_reloaded);
        }), 2);
    EXPECT_EQ(std::count_if(snapshot.api_metadata.begin(),
                            snapshot.api_metadata.end(),
        [&](const auto& received) {
            return received.message.apiinfo() == api_key &&
                   received.message.type() == API_TYPE_INVOCATION &&
                   received.message.apiid() == api_invocation;
        }), 1);
    EXPECT_EQ(std::count_if(snapshot.string_metadata.begin(),
                            snapshot.string_metadata.end(),
        [&](const auto& received) {
            return received.message.stringvalue() == error_key &&
                   (received.message.stringid() == error_id ||
                    received.message.stringid() == error_reloaded);
        }), 2);
    EXPECT_EQ(std::count_if(snapshot.sql_metadata.begin(),
                            snapshot.sql_metadata.end(),
        [&](const auto& received) {
            return received.message.sql() == sql_key &&
                   (received.message.sqlid() == sql_id ||
                    received.message.sqlid() == sql_reloaded);
        }), 2);

    std::vector<std::string> received_uids;
    for (const auto& received : snapshot.sql_uid_metadata) {
        if (received.message.sql() == sql_uid_key) {
            received_uids.push_back(received.message.sqluid());
        }
    }
    ASSERT_EQ(received_uids.size(), 2U);
    EXPECT_EQ(received_uids[0].size(), 16U);
    EXPECT_EQ(received_uids[0], received_uids[1]);
}

TEST_F(AgentIntegrationTest, ReportsResponseTimeAndRuntimeStatistics) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return agent_stat_count(snapshot) >= 1;
    }, kWaitTimeout));
    const auto baseline = agent_stat_count(collector_.snapshot());

    const std::array<std::chrono::milliseconds, 3> elapsed_times{
        80ms, 240ms, 480ms};
    for (size_t i = 0; i < elapsed_times.size(); ++i) {
        auto span = agent_->NewSpan("stat.response." + std::to_string(i),
                                    "/stat-response/" + std::to_string(i));
        ASSERT_TRUE(span->IsSampled());
        span->SetStartTime(std::chrono::system_clock::now() - elapsed_times[i]);
        span->EndSpan();
    }

    ASSERT_TRUE(collector_.WaitFor([baseline](const auto& snapshot) {
        const auto totals = transaction_totals_after(snapshot, baseline);
        return totals.sampled_new >= 3 &&
               max_response_time_after(snapshot, baseline) >= 400;
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto totals = transaction_totals_after(snapshot, baseline);
    EXPECT_EQ(totals.sampled_new, 3);
    EXPECT_EQ(totals.unsampled_new, 0);
    EXPECT_EQ(totals.skipped_new, 0);

    std::optional<v1::PAgentStat> runtime_stat;
    size_t index = 0;
    for (const auto& received : snapshot.stats) {
        if (!received.message.has_agentstatbatch()) {
            continue;
        }
        for (const auto& stat : received.message.agentstatbatch().agentstat()) {
            if (index++ >= baseline && stat.has_responsetime() &&
                stat.responsetime().max() >= 400) {
                runtime_stat = stat;
                break;
            }
        }
        if (runtime_stat.has_value()) {
            break;
        }
    }

    ASSERT_TRUE(runtime_stat.has_value());
    EXPECT_GT(runtime_stat->responsetime().avg(), 0);
    EXPECT_GE(runtime_stat->responsetime().max(), 400);
    ASSERT_TRUE(runtime_stat->has_cpuload());
    EXPECT_GE(runtime_stat->cpuload().jvmcpuload(), 0.0);
    EXPECT_LE(runtime_stat->cpuload().jvmcpuload(), 1.0);
    EXPECT_GE(runtime_stat->cpuload().systemcpuload(), 0.0);
    EXPECT_LE(runtime_stat->cpuload().systemcpuload(), 1.0);
    ASSERT_TRUE(runtime_stat->has_gc());
    EXPECT_GE(runtime_stat->gc().jvmmemoryheapused(), 0);
    EXPECT_GE(runtime_stat->gc().jvmmemoryheapmax(), 0);
    ASSERT_TRUE(runtime_stat->has_totalthread());
    EXPECT_GT(runtime_stat->totalthread().totalthreadcount(), 0);
}

TEST_F(AgentIntegrationTest,
       AppliesCounterAndParentSamplingAndReportsDecisions) {
    cfg_.sampling_counter_rate = 3;
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return agent_stat_count(snapshot) >= 1;
    }, kWaitTimeout));
    const auto baseline = agent_stat_count(collector_.snapshot());

    // The counter is tested before it is incremented, so the first
    // transaction after startup is sampled and then every third one.
    const std::array<bool, 6> expected{true, false, false,
                                       true, false, false};
    const auto sampled_trace_id = DriveSamplingPattern(
        "sampling.counter", "/sampling/counter/", expected);
    ASSERT_FALSE(sampled_trace_id.empty());

    MapCarrier continued_context;
    continued_context.Set(HEADER_TRACE_ID, sampled_trace_id);
    // All three headers, or this is not a continued trace at all and the
    // new-trace sampler decides it (see doc/api_contracts.md).
    continued_context.Set(HEADER_SPAN_ID, "77777");
    continued_context.Set(HEADER_PARENT_SPAN_ID, "88888");
    auto continued = agent_->NewSpan("sampling.continued",
                                     "/sampling/continued",
                                     continued_context);
    EXPECT_TRUE(continued->IsSampled());
    continued->EndSpan();

    MapCarrier unsampled_context;
    unsampled_context.Set(HEADER_SAMPLED, "s0");
    auto unsampled = agent_->NewSpan("sampling.parent-denied",
                                     "/sampling/parent-denied",
                                     unsampled_context);
    EXPECT_FALSE(unsampled->IsSampled());
    unsampled->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([baseline](const auto& snapshot) {
        const auto totals = transaction_totals_after(snapshot, baseline);
        return totals.sampled_new >= 2 && totals.unsampled_new >= 4 &&
               totals.sampled_continuation >= 1 &&
               totals.unsampled_continuation >= 1 &&
               count_spans_by_rpc(snapshot, "/sampling/continued") == 1;
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto totals = transaction_totals_after(snapshot, baseline);
    EXPECT_EQ(totals.sampled_new, 2);
    EXPECT_EQ(totals.unsampled_new, 4);
    EXPECT_EQ(totals.sampled_continuation, 1);
    EXPECT_EQ(totals.unsampled_continuation, 1);
    EXPECT_EQ(totals.skipped_new, 0);
    EXPECT_EQ(totals.skipped_continuation, 0);
    ExpectSamplingPattern(snapshot, "/sampling/counter/", expected);
    EXPECT_EQ(count_spans_by_rpc(snapshot, "/sampling/continued"), 1U);
    EXPECT_EQ(count_spans_by_rpc(snapshot, "/sampling/parent-denied"), 0U);
}

TEST_F(AgentIntegrationTest,
       EnforcesNewAndContinuationThroughputLimits) {
    cfg_.sampling_new_throughput = 2;
    cfg_.sampling_continue_throughput = 1;
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return agent_stat_count(snapshot) >= 1;
    }, kWaitTimeout));
    const auto baseline = agent_stat_count(collector_.snapshot());

    // Both buckets fill from agent construction and cap at one second of
    // tokens, so idling past that cap makes the bursts below deterministic
    // however long the startup handshake took: each bucket holds exactly its
    // throughput. A burst then admits those stored tokens plus one more — the
    // caller that finds the bucket empty but its next token already due
    // borrows it — which is what Guava's SmoothBursty, and so the Java agent,
    // does with the same setting.
    std::this_thread::sleep_for(1200ms);

    const std::array<bool, 5> expected_new{true, true, true, false, false};
    const auto parent_trace_id = DriveSamplingPattern(
        "sampling.throughput.new", "/sampling/throughput/new/", expected_new);
    ASSERT_FALSE(parent_trace_id.empty());

    const std::array<bool, 3> expected_continuation{true, true, false};
    MapCarrier context;
    context.Set(HEADER_TRACE_ID, parent_trace_id);
    context.Set(HEADER_SPAN_ID, "77777");
    context.Set(HEADER_PARENT_SPAN_ID, "88888");
    DriveSamplingPattern("sampling.throughput.continued",
                         "/sampling/throughput/continued/",
                         expected_continuation, &context);

    ASSERT_TRUE(collector_.WaitFor([baseline](const auto& snapshot) {
        const auto totals = transaction_totals_after(snapshot, baseline);
        return totals.sampled_new >= 3 && totals.skipped_new >= 2 &&
               totals.sampled_continuation >= 2 &&
               totals.skipped_continuation >= 1 &&
               count_spans_by_rpc(
                   snapshot, "/sampling/throughput/continued/0") == 1;
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto totals = transaction_totals_after(snapshot, baseline);
    EXPECT_EQ(totals.sampled_new, 3);
    EXPECT_EQ(totals.skipped_new, 2);
    EXPECT_EQ(totals.unsampled_new, 0);
    EXPECT_EQ(totals.sampled_continuation, 2);
    EXPECT_EQ(totals.skipped_continuation, 1);
    ExpectSamplingPattern(snapshot, "/sampling/throughput/new/", expected_new);
    ExpectSamplingPattern(snapshot, "/sampling/throughput/continued/",
                          expected_continuation);
}

TEST_F(AgentIntegrationTest,
       NormalizesAndAggregatesUrlStatisticsAndFailures) {
    cfg_.url_stat_enable_trim_path = true;
    cfg_.url_stat_trim_path_depth = 2;
    ASSERT_NO_FATAL_FAILURE(StartStack());

    auto success = agent_->NewSpan("url.stat.success", "/url-stat/success");
    ASSERT_TRUE(success->IsSampled());
    success->SetStartTime(std::chrono::system_clock::now() - 50ms);
    success->SetUrlStat("/api/orders/42/items?debug=1", "GET", 200);
    success->SetStatusCode(200);
    success->EndSpan();

    auto failure = agent_->NewSpan("url.stat.failure", "/url-stat/failure");
    ASSERT_TRUE(failure->IsSampled());
    failure->SetStartTime(std::chrono::system_clock::now() - 350ms);
    failure->SetUrlStat("/api/orders/99/items", "GET", 503);
    failure->SetStatusCode(503);
    failure->EndSpan();

    constexpr std::string_view normalized_uri = "GET /api/orders/*";
    ASSERT_TRUE(FlushUrlStatsUntil(normalized_uri, 2));

    const auto snapshot = collector_.snapshot();
    const auto totals = uri_stat_totals(snapshot, normalized_uri);
    EXPECT_GE(totals.entries, 1U);
    EXPECT_EQ(totals.total_count, 2);
    EXPECT_EQ(totals.failed_count, 1);
    EXPECT_GE(totals.total_elapsed, 400);
    EXPECT_GE(totals.max_elapsed, 350);
    EXPECT_GE(totals.failed_elapsed, 350);
    EXPECT_GE(totals.failed_max_elapsed, 350);
    EXPECT_FALSE(has_uri_stat(snapshot, "GET /api/orders/42/items?debug=1"));
    EXPECT_FALSE(has_uri_stat(snapshot, "GET /api/orders/99/items"));
}

// A five-minute stat-stream outage, end to end over real gRPC. The tick
// interval is a fixed 30s in production, so the ten ticks are supplied as
// synthetic span end times through the live agent's UrlStats rather than by
// waiting five real minutes; everything downstream of that — tick bucketing,
// the retention queue, the snapshot swap on the send token, the protobuf
// encoding and the stream itself — is the production path.
//
// Before tick-boundary snapshots, one limit-capped map absorbed the whole
// outage: the collector would have received url_stat_limit entries in total
// and every tick after the first would have been starved.
TEST_F(AgentIntegrationTest, KeepsPerTickUrlStatisticsThroughAStatStreamOutage) {
    constexpr int kLimit = 3;
    constexpr int kUrlsPerTick = 5;   // more than kLimit, so each tick overflows
    constexpr int kStalledTicks = 10; // 10 x 30s = the five-minute outage
    constexpr int64_t kTickMillis = 30000;
    // Tick-aligned so the expected tick values are exact.
    constexpr int64_t kBaseMillis = 1700000010000;
    static_assert(kBaseMillis % kTickMillis == 0, "base must sit on a tick boundary");

    cfg_.url_stat_limit = kLimit;
    cfg_.url_stat_enable_trim_path = false;
    ASSERT_NO_FATAL_FAILURE(StartStack());

    // The stat port stops listening: the stream dies and cannot be reopened,
    // so no send token can consume a snapshot until the port comes back.
    ASSERT_TRUE(collector_.StopEndpoint(CollectorEndpoint::Stat));

    const auto config = impl_->getConfig();
    auto& url_stats = impl_->getUrlStats();
    for (int tick = 0; tick < kStalledTicks; ++tick) {
        const auto end_time = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(kBaseMillis + tick * kTickMillis));
        for (int url = 0; url < kUrlsPerTick; ++url) {
            UrlStatEntry entry("/stall/tick" + std::to_string(tick) + "/url" +
                                   std::to_string(url),
                               "GET", 200);
            entry.elapsed_ = 10;
            entry.end_time_ = end_time;
            url_stats.addSnapshot(&entry, *config);
        }
    }

    // Wait for the stat stream to actually reopen before asking for a send:
    // next_write() takes the snapshot when it consumes the token, so a token
    // driven into a stream that is still down would discard the whole outage.
    const auto streams_before = collector_.snapshot().stat_streams.size();
    ASSERT_TRUE(collector_.StartEndpoint(CollectorEndpoint::Stat));
    ASSERT_TRUE(collector_.WaitFor([streams_before](const auto& snapshot) {
        return snapshot.stat_streams.size() > streams_before;
    }, std::chrono::seconds(20))) << "the stat stream never came back";

    const auto stalled_entries = [](const CollectorSnapshot& snapshot) {
        std::map<int64_t, std::set<std::string>> by_tick;
        for (const auto& received : snapshot.stats) {
            if (!received.message.has_agenturistat()) {
                continue;
            }
            for (const auto& stat : received.message.agenturistat().eachuristat()) {
                // The fixture enables UrlStatMethodPrefix, so keys read
                // "GET /stall/tick<n>/url<n>".
                if (stat.uri().rfind("GET /stall/tick", 0) == 0) {
                    by_tick[stat.timestamp()].insert(stat.uri());
                }
            }
        }
        return by_tick;
    };
    // Stats are only sent on a token; the agent's own 30s timer would outlast
    // the test, so drive it the way FlushUrlStatsUntil does.
    bool delivered = false;
    for (int attempt = 0; attempt < 20 && !delivered; ++attempt) {
        impl_->recordStats(URL_STATS);
        delivered = collector_.WaitFor([&stalled_entries](const auto& snapshot) {
            return stalled_entries(snapshot).size() >= 5;
        }, 250ms);
    }
    ASSERT_TRUE(delivered) << "retained ticks never reached the collector";

    const auto by_tick = stalled_entries(collector_.snapshot());

    // Four completed ticks are retained plus the one still in progress; the
    // older six were evicted whole rather than starving the newer ones.
    ASSERT_EQ(by_tick.size(), 5U);
    int expected_tick = kStalledTicks - 5;
    for (const auto& [timestamp, uris] : by_tick) {
        EXPECT_EQ(timestamp, kBaseMillis + expected_tick * kTickMillis)
            << "ticks must be reported on the unchanged 30s grid";
        EXPECT_EQ(uris.size(), static_cast<size_t>(kLimit))
            << "tick " << timestamp << " must get the full limit, not a share of it";
        for (const auto& uri : uris) {
            EXPECT_EQ(uri.rfind("GET /stall/tick" + std::to_string(expected_tick) + "/", 0), 0U)
                << uri << " was reported under the wrong tick";
        }
        ++expected_tick;
    }
    EXPECT_FALSE(has_uri_stat(collector_.snapshot(), "GET /stall/tick0/url0"))
        << "the oldest tick is the one evicted when retention overflows";
}

TEST_F(AgentIntegrationTest, HandlesProfilerCommandsOverRealGrpcStreams) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return !snapshot.command_streams_v2.empty();
    }, kWaitTimeout));

    collector_.SendEchoCommand(101, "collector-echo");
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return std::any_of(snapshot.echo_responses.begin(), snapshot.echo_responses.end(),
            [](const auto& response) {
                return response.message.commonresponse().responseid() == 101;
            });
    }, kWaitTimeout));

    auto active = agent_->NewSpan("command.active", "/command-active");
    ASSERT_TRUE(active->IsSampled());
    collector_.SendActiveThreadCountCommand(102);
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return count_active_thread_responses(snapshot, 102) >= 1;
    }, kWaitTimeout));

    // Dump commands are intentionally unsupported by the C++ agent. Sending
    // one proves that the bidirectional stream records the fail protobuf too.
    collector_.SendActiveThreadDumpCommand(103, 3);
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return std::any_of(snapshot.command_stream_messages.begin(),
                           snapshot.command_stream_messages.end(),
            [](const auto& response) {
                return response.message.has_failmessage() &&
                       response.message.failmessage().responseid() == 103;
            });
    }, kWaitTimeout));
    active->EndSpan();

    const auto snapshot = collector_.snapshot();
    const auto echo = std::find_if(snapshot.echo_responses.begin(),
                                   snapshot.echo_responses.end(),
        [](const auto& response) {
            return response.message.commonresponse().responseid() == 101;
        });
    ASSERT_NE(echo, snapshot.echo_responses.end());
    EXPECT_EQ(echo->message.message(), "collector-echo");
    expect_common_metadata(echo->metadata, false, impl_->getAgentId());

    const auto active_count = std::find_if(
        snapshot.active_thread_count_responses.begin(),
        snapshot.active_thread_count_responses.end(),
        [](const auto& response) {
            return response.message.commonstreamresponse().responseid() == 102;
        });
    ASSERT_NE(active_count, snapshot.active_thread_count_responses.end());
    EXPECT_EQ(active_count->message.commonstreamresponse().sequenceid(), 1);
    EXPECT_EQ(active_count->message.histogramschematype(), 2);
    ASSERT_EQ(active_count->message.activethreadcount_size(), 4);
    EXPECT_GE(std::accumulate(active_count->message.activethreadcount().begin(),
                              active_count->message.activethreadcount().end(), 0),
              1);
    EXPECT_GT(active_count->message.timestamp(), 0);
    expect_common_metadata(active_count->metadata, true, impl_->getAgentId());

    const auto failure = std::find_if(snapshot.command_stream_messages.begin(),
                                      snapshot.command_stream_messages.end(),
        [](const auto& response) {
            return response.message.has_failmessage() &&
                   response.message.failmessage().responseid() == 103;
        });
    ASSERT_NE(failure, snapshot.command_stream_messages.end());
    EXPECT_EQ(failure->message.failmessage().message().value(),
              "NOT_SUPPORTED_REQUEST");
}

TEST_F(AgentIntegrationTest, DroppedSpanReleasesActiveRequestWithoutSendingSpan) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return !snapshot.command_streams_v2.empty();
    }, kWaitTimeout));

    auto dropped = agent_->NewSpan("dropped.request", "/dropped-without-end");
    ASSERT_TRUE(dropped->IsSampled());
    constexpr int32_t kRequestId = 150;
    collector_.SendActiveThreadCountCommand(kRequestId);
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return std::any_of(
            snapshot.active_thread_count_responses.begin(),
            snapshot.active_thread_count_responses.end(),
            [](const auto& response) {
                if (response.message.commonstreamresponse().responseid() !=
                    kRequestId) {
                    return false;
                }
                return std::accumulate(
                           response.message.activethreadcount().begin(),
                           response.message.activethreadcount().end(), 0) >= 1;
            });
    }, kWaitTimeout));

    // User code may abandon a span on an early return or exception. Its
    // destructor must remove the active-request entry even though no PSpan is
    // finalized or sent.
    dropped.reset();
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return std::any_of(
            snapshot.active_thread_count_responses.begin(),
            snapshot.active_thread_count_responses.end(),
            [](const auto& response) {
                if (response.message.commonstreamresponse().responseid() !=
                        kRequestId ||
                    response.message.commonstreamresponse().sequenceid() < 2) {
                    return false;
                }
                return std::accumulate(
                           response.message.activethreadcount().begin(),
                           response.message.activethreadcount().end(), 0) == 0;
            });
    }, kWaitTimeout));

    EXPECT_EQ(count_spans_by_rpc(collector_.snapshot(),
                                 "/dropped-without-end"),
              0U);
}

// UNAVAILABLE is transient, so the item is rescheduled; an application-level
// rejection is not retried at all. Both recover the same way in the end: the
// dropped item releases its cache entry, so the next use of the name
// re-registers it under a fresh id and sends a genuinely new request.
TEST_F(AgentIntegrationTest, RetriesTransientMetadataFailureButNotRejection) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    collector_.FailNext(CollectorRpc::ApiMetadata,
                        grpc::StatusCode::UNAVAILABLE,
                        "metadata endpoint unavailable");
    collector_.RejectNext(CollectorRpc::ApiMetadata,
                          "collector rejected metadata");

    const auto api_id = impl_->cacheApi("fault.retry.api", API_TYPE_DEFAULT);
    ASSERT_GT(api_id, 0);

    // Attempt 1 fails with UNAVAILABLE and is retried; attempt 2 comes back
    // rejected and ends the item there.
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return results_for(snapshot, CollectorRpc::ApiMetadata).size() >= 2;
    }, kWaitTimeout));

    // The rejection released the cache entry, so the same name now yields a
    // new id and publishes again. Polled because the release happens on the
    // metadata worker; every call before it is a plain cache hit that sends
    // nothing, so exactly one extra request follows.
    int32_t resent_id = api_id;
    ASSERT_TRUE(wait_until([&] {
        resent_id = impl_->cacheApi("fault.retry.api", API_TYPE_DEFAULT);
        return resent_id != api_id;
    }));
    ASSERT_GT(resent_id, 0);

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return results_for(snapshot, CollectorRpc::ApiMetadata).size() >= 3;
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto results = results_for(snapshot, CollectorRpc::ApiMetadata);
    ASSERT_EQ(results.size(), 3U);
    EXPECT_EQ(results[0].status_code, grpc::StatusCode::UNAVAILABLE);
    EXPECT_FALSE(results[0].response_success);
    EXPECT_EQ(results[1].status_code, grpc::StatusCode::OK);
    EXPECT_FALSE(results[1].response_success);
    EXPECT_EQ(results[2].status_code, grpc::StatusCode::OK);
    EXPECT_TRUE(results[2].response_success);

    // Three requests for the name: the failed send, its retry, and the
    // re-registration — the rejection itself was never resent.
    std::vector<int32_t> sent_ids;
    for (const auto& received : snapshot.api_metadata) {
        if (received.message.apiinfo() == "fault.retry.api") {
            sent_ids.push_back(received.message.apiid());
        }
    }
    ASSERT_EQ(sent_ids.size(), 3U);
    EXPECT_EQ(sent_ids[0], api_id);
    EXPECT_EQ(sent_ids[1], api_id);
    EXPECT_EQ(sent_ids[2], resent_id);
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(AgentIntegrationTest, TimesOutCommandRequestAndKeepsStreamUsable) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return !snapshot.command_streams_v2.empty();
    }, kWaitTimeout));

    collector_.TimeoutNext(CollectorRpc::CommandEcho);
    collector_.SendEchoCommand(201, "will-time-out");

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        const auto timed_out = has_result(
            snapshot, CollectorRpc::CommandEcho,
            grpc::StatusCode::DEADLINE_EXCEEDED, false);
        const auto failure_sent = std::any_of(
            snapshot.command_stream_messages.begin(),
            snapshot.command_stream_messages.end(),
            [](const auto& message) {
                return message.message.has_failmessage() &&
                       message.message.failmessage().responseid() == 201;
            });
        return timed_out && failure_sent;
    }, kWaitTimeout));

    // A timed-out unary response must not tear down the command bidi stream.
    collector_.SendEchoCommand(202, "after-timeout");
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return std::any_of(snapshot.echo_responses.begin(),
                           snapshot.echo_responses.end(),
            [](const auto& response) {
                return response.message.commonresponse().responseid() == 202;
            }) && has_result(snapshot, CollectorRpc::CommandEcho,
                             grpc::StatusCode::OK, true);
    }, kWaitTimeout));
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(AgentIntegrationTest, ContinuesSendingAfterSpanRequestError) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    collector_.FailNext(CollectorRpc::SendSpanBatch,
                        grpc::StatusCode::INTERNAL,
                        "span batch rejected");

    auto failed_span = agent_->NewSpan("faulted.span", "/faulted-span");
    ASSERT_TRUE(failed_span->IsSampled());
    failed_span->EndSpan();
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/faulted-span").has_value() &&
               has_result(snapshot, CollectorRpc::SendSpanBatch,
                          grpc::StatusCode::INTERNAL, false);
    }, kWaitTimeout));

    auto healthy_span = agent_->NewSpan("healthy.span", "/healthy-span");
    ASSERT_TRUE(healthy_span->IsSampled());
    healthy_span->EndSpan();
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/healthy-span").has_value() &&
               has_result(snapshot, CollectorRpc::SendSpanBatch,
                          grpc::StatusCode::OK, true);
    }, kWaitTimeout));
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(AgentIntegrationTest, ReconnectsAfterEndpointAndCommandStreamFailures) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return !snapshot.ping_streams.empty() &&
               !snapshot.command_streams_v2.empty();
    }, kWaitTimeout));
    const auto before = collector_.snapshot();

    // This closes the listening socket and all live Agent/Metadata/Command
    // HTTP/2 connections, then brings the exact same port back.
    ASSERT_TRUE(collector_.StopEndpoint(CollectorEndpoint::Agent));
    collector_.FailNext(CollectorRpc::HandleCommandV2,
                        grpc::StatusCode::UNAVAILABLE,
                        "command stream rejected after reconnect");
    ASSERT_TRUE(collector_.StartEndpoint(CollectorEndpoint::Agent));

    ASSERT_TRUE(collector_.WaitFor([&before](const auto& snapshot) {
        return snapshot.command_streams_v2.size() >=
                   before.command_streams_v2.size() + 2 &&
               has_result(snapshot, CollectorRpc::HandleCommandV2,
                          grpc::StatusCode::UNAVAILABLE, false);
    }, std::chrono::seconds(20)));

    collector_.SendEchoCommand(303, "after-reconnect");
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return std::any_of(snapshot.echo_responses.begin(),
                           snapshot.echo_responses.end(),
            [](const auto& response) {
                return response.message.commonresponse().responseid() == 303;
            });
    }, kWaitTimeout));

    // Exercise a separate transport channel outage as well. A span queued
    // during the outage may be dropped by policy, but later traffic must flow.
    ASSERT_TRUE(collector_.StopEndpoint(CollectorEndpoint::Span));
    auto outage_span = agent_->NewSpan("span.during.outage", "/span-during-outage");
    ASSERT_TRUE(outage_span->IsSampled());
    outage_span->EndSpan();
    std::this_thread::sleep_for(100ms);
    ASSERT_TRUE(collector_.StartEndpoint(CollectorEndpoint::Span));

    auto recovered_span = agent_->NewSpan("span.after.reconnect", "/span-after-reconnect");
    ASSERT_TRUE(recovered_span->IsSampled());
    recovered_span->EndSpan();
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/span-after-reconnect").has_value();
    }, std::chrono::seconds(20)));
    EXPECT_TRUE(agent_->Enable());
}

// Steady traffic across several renewal periods: the span, metadata, stat and
// command channels all move to new connections (distinct peers) while every
// span and every metadata item still arrives exactly once, and the stat and
// command streams are reopened on the renewed channels.
TEST_F(ConnectionRenewalIntegrationTest, RenewsChannelsAndStreamsWithoutLosingData) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    // ~1.5s of traffic, i.e. several 300ms periods. Each span carries its own
    // operation name so every one of them also produces one ApiMetaData RPC
    // over the metadata channel.
    constexpr int kSpans = 60;
    for (int i = 0; i < kSpans; ++i) {
        auto span = agent_->NewSpan("renewal.op." + std::to_string(i),
                                    "/renewal/" + std::to_string(i));
        ASSERT_TRUE(span->IsSampled());
        span->EndSpan();
        std::this_thread::sleep_for(25ms);
    }

    const auto renewal_spans = [](const CollectorSnapshot& snapshot) {
        size_t count = 0;
        for (const auto& message : all_span_messages(snapshot)) {
            if (message.has_span() && message.span().has_acceptevent() &&
                message.span().acceptevent().rpc().rfind("/renewal/", 0) == 0) {
                ++count;
            }
        }
        return count;
    };
    const auto renewal_apis = [](const CollectorSnapshot& snapshot) {
        std::set<std::string> apis;
        for (const auto& received : snapshot.api_metadata) {
            if (received.message.apiinfo().rfind("renewal.op.", 0) == 0) {
                apis.insert(received.message.apiinfo());
            }
        }
        return apis.size();
    };
    ASSERT_TRUE(collector_.WaitFor([&](const auto& snapshot) {
        return renewal_spans(snapshot) >= kSpans &&
               renewal_apis(snapshot) >= kSpans &&
               distinct_peers(snapshot.stat_streams) >= 2 &&
               distinct_peers(snapshot.command_streams_v2) >= 2;
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    EXPECT_EQ(renewal_spans(snapshot), static_cast<size_t>(kSpans))
        << "every span must arrive exactly once across channel rotations";
    EXPECT_EQ(renewal_apis(snapshot), static_cast<size_t>(kSpans))
        << "every metadata item must arrive across channel rotations";
    EXPECT_GE(distinct_peers(snapshot.span_batches), 2u)
        << "span batches must arrive over more than one connection";
    EXPECT_GE(distinct_peers(snapshot.api_metadata), 2u)
        << "metadata must arrive over more than one connection";
    EXPECT_GE(snapshot.stat_streams.size(), 2u)
        << "the stat stream must be reopened once it reaches its max age";
    EXPECT_GE(snapshot.command_streams_v2.size(), 2u)
        << "the command stream must be reopened once its deadline expires";
    EXPECT_TRUE(agent_->Enable());
}

// ChannelMaxAgeMs by itself must close the long-lived stat and command
// streams at a safe boundary, giving readyChannel() a chance to rotate.
TEST_F(ChannelOnlyRenewalIntegrationTest, RotatesStatAndCommandChannels) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return distinct_peers(snapshot.stat_streams) >= 2 &&
               distinct_peers(snapshot.command_streams_v2) >= 2;
    }, kWaitTimeout));
    EXPECT_TRUE(agent_->Enable());
}

// The ping stream checks its renewal deadline once per ping interval, which
// production sets to 60s. Use a short injected interval to prove that
// ChannelMaxAgeMs alone reopens the stream and rotates the agent channel.
TEST_F(ChannelOnlyRenewalIntegrationTest, ReopensPingStreamAndRotatesAgentChannel) {
    ASSERT_NO_FATAL_FAILURE(StartCollector());
    auto config = make_config(agent_options());
    ASSERT_NE(config, nullptr);

    GrpcClientTuning tuning;
    tuning.ping_interval = 50ms;
    auto agent = AgentImpl::createShared(
        config,
        std::make_unique<GrpcAgent>(config, tuning),
        std::make_unique<GrpcMetadata>(config, tuning),
        std::make_unique<GrpcSpan>(config, tuning),
        std::make_unique<GrpcStats>(config, tuning),
        nullptr, kApplicationType);
    ASSERT_TRUE(agent->Start());

    // Session 1 from startup, then one reopen after the 300ms (+-10%) channel
    // deadline. The reopened session must arrive over a new connection even
    // though StreamMaxAgeMs is disabled.
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return snapshot.ping_streams.size() >= 2 &&
               distinct_peers(snapshot.ping_streams) >= 2;
    }, kWaitTimeout));
    EXPECT_TRUE(agent->Enable());

    agent->Shutdown();
}

TEST_F(AgentIntegrationTest, ReconnectsStatStreamAfterServerError) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return !snapshot.stat_streams.empty();
    }, kWaitTimeout));
    const auto initial = collector_.snapshot();

    // This is consumed by the already-open stream after its next message.
    collector_.FailNext(CollectorRpc::SendAgentStat,
                        grpc::StatusCode::UNAVAILABLE,
                        "stat stream closed by collector");
    impl_->recordStats(AGENT_STATS);
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return has_result(snapshot, CollectorRpc::SendAgentStat,
                          grpc::StatusCode::UNAVAILABLE, false);
    }, kWaitTimeout));

    // One more write surfaces the closed stream to the client reactor; it must
    // then establish a fresh SendAgentStat stream.
    impl_->recordStats(AGENT_STATS);
    ASSERT_TRUE(collector_.WaitFor([&initial](const auto& snapshot) {
        return snapshot.stat_streams.size() >= initial.stat_streams.size() + 1;
    }, kWaitTimeout));

    const auto received_before_healthy_write = collector_.snapshot().stats.size();
    impl_->recordStats(AGENT_STATS);
    ASSERT_TRUE(collector_.WaitFor([received_before_healthy_write](const auto& snapshot) {
        return snapshot.stats.size() > received_before_healthy_write;
    }, kWaitTimeout));
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(AgentIntegrationTest, ShutdownCancelsTimedOutStatStream) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return !snapshot.stat_streams.empty();
    }, kWaitTimeout));
    const auto stats_before = collector_.snapshot().stats.size();

    // The already-open stream accepts this message, then deliberately stops
    // completing the RPC until the client cancels it.
    collector_.TimeoutNext(CollectorRpc::SendAgentStat);
    impl_->recordStats(AGENT_STATS);
    ASSERT_TRUE(collector_.WaitFor([stats_before](const auto& snapshot) {
        return snapshot.stats.size() > stats_before;
    }, kWaitTimeout));

    const auto started = std::chrono::steady_clock::now();
    agent_->Shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::seconds(8));
    EXPECT_FALSE(agent_->Enable());
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return has_result(snapshot, CollectorRpc::SendAgentStat,
                          grpc::StatusCode::DEADLINE_EXCEEDED, false);
    }, 2s));
}

TEST_F(AgentInfoRetryIntegrationTest, RetriesAgentRegistrationAfterInitialFailure) {
    // StartStack() blocks until registration succeeded and the agent came
    // online, so by now the failed first attempt and its retry are on record.
    ASSERT_NO_FATAL_FAILURE(StartStack());

    const auto snapshot = collector_.snapshot();
    ASSERT_GE(snapshot.agent_infos.size(), 2U);
    EXPECT_EQ(snapshot.agent_infos[0].message.agentversion(),
              snapshot.agent_infos[1].message.agentversion());

    const auto results = results_for(snapshot, CollectorRpc::AgentInfo);
    ASSERT_GE(results.size(), 2U);
    EXPECT_EQ(results[0].status_code, grpc::StatusCode::UNAVAILABLE);
    EXPECT_FALSE(results[0].response_success);
    EXPECT_EQ(results[1].status_code, grpc::StatusCode::OK);
    EXPECT_TRUE(results[1].response_success);
    EXPECT_TRUE(agent_->Enable());
}

// Boot registration retries forever, but the periodic AgentInfo re-sender is
// bounded by MaxTryPerAttempt and a failed cycle is best-effort: it must retry
// within the cycle and leave the agent enabled either way. The other retry
// tests here all cover the boot path, which is a different loop.
TEST_F(AgentIntegrationTest, RetriesPeriodicAgentInfoResendAfterFailure) {
    // Short enough that a refresh lands during the test; the retry interval
    // and attempt count come from the fixture (50ms, 2 tries).
    cfg_.agent_info_refresh_interval_ms = 200;
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_GE(collector_.snapshot().agent_infos.size(), 1U);

    // Armed only now, so boot registration keeps its own success and the
    // fault lands on a re-send instead.
    collector_.FailNext(CollectorRpc::AgentInfo,
                        grpc::StatusCode::UNAVAILABLE,
                        "periodic re-send rejected");

    // The failed attempt and its retry both reach the collector. Locate the
    // injected failure instead of assuming its index: a periodic re-send can
    // land between the snapshot above and FailNext arming, shifting every
    // later entry by one.
    const auto find_failed = [](const std::vector<RpcResult>& results) {
        return std::find_if(results.begin(), results.end(), [](const RpcResult& r) {
            return r.status_code == grpc::StatusCode::UNAVAILABLE;
        });
    };
    ASSERT_TRUE(collector_.WaitFor([&find_failed](const auto& snapshot) {
        const auto results = results_for(snapshot, CollectorRpc::AgentInfo);
        const auto failed = find_failed(results);
        return failed != results.end() && std::next(failed) != results.end();
    }, kWaitTimeout));

    const auto results = results_for(collector_.snapshot(), CollectorRpc::AgentInfo);
    const auto failed = find_failed(results);
    ASSERT_NE(failed, results.end());
    EXPECT_FALSE(failed->response_success);
    const auto& retried = *std::next(failed);
    EXPECT_EQ(retried.status_code, grpc::StatusCode::OK);
    EXPECT_TRUE(retried.response_success);
    // A best-effort cycle must never take the agent offline.
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(CollectorUnavailableAtStartupIntegrationTest,
       EnablesAndStartsAllGrpcWorkersAfterCollectorRecovery) {
    // The init thread may keep retrying indefinitely, but it must not expose a
    // half-started agent or start any downstream worker before AgentInfo is
    // accepted.
    std::this_thread::sleep_for(300ms);
    EXPECT_FALSE(agent_->Enable());
    const auto outage_snapshot = collector_.snapshot();
    EXPECT_TRUE(outage_snapshot.agent_infos.empty());
    EXPECT_TRUE(outage_snapshot.ping_streams.empty());
    EXPECT_TRUE(outage_snapshot.command_streams_v2.empty());
    EXPECT_TRUE(outage_snapshot.stat_streams.empty());

    ASSERT_NO_FATAL_FAILURE(RestartCollector());
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return !snapshot.agent_infos.empty() &&
               !snapshot.pings.empty() &&
               !snapshot.command_streams_v2.empty() &&
               !snapshot.stat_streams.empty();
    }, std::chrono::seconds(20)));
    ASSERT_TRUE(wait_until([this] { return agent_->Enable(); },
                           std::chrono::seconds(20)));

    // Verify more than registration: every independent collector channel must
    // carry fresh work after the full outage ends.
    constexpr std::string_view api_info = "collector.startup.recovery.api";
    const auto api_id = impl_->cacheApi(api_info, API_TYPE_DEFAULT);
    ASSERT_GT(api_id, 0);

    const auto stats_before = collector_.snapshot().stats.size();
    impl_->recordStats(AGENT_STATS);

    auto span = agent_->NewSpan("collector.startup.recovery",
                                "/collector-startup-recovery");
    ASSERT_TRUE(span->IsSampled());
    span->EndSpan();

    collector_.SendEchoCommand(401, "collector-recovered");
    ASSERT_TRUE(collector_.WaitFor([stats_before](const auto& snapshot) {
        const auto echo_received = std::any_of(
            snapshot.echo_responses.begin(), snapshot.echo_responses.end(),
            [](const auto& response) {
                return response.message.commonresponse().responseid() == 401;
            });
        return find_span_by_rpc(snapshot, "/collector-startup-recovery").has_value() &&
               has_api_metadata(snapshot, "collector.startup.recovery.api",
                                API_TYPE_DEFAULT) &&
               snapshot.stats.size() > stats_before &&
               echo_received;
    }, std::chrono::seconds(20)));
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(CollectorUnavailableAtStartupIntegrationTest,
       ShutdownInterruptsInitialCollectorWait) {
    // Give the init thread time to enter the channel-readiness backoff while
    // all three collector endpoints remain unavailable.
    std::this_thread::sleep_for(300ms);
    ASSERT_FALSE(agent_->Enable());

    const auto started = std::chrono::steady_clock::now();
    agent_->Shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::seconds(3))
        << "shutdown must interrupt the initial collector wait, not wait for a request deadline";
    EXPECT_FALSE(agent_->Enable());

    const auto snapshot = collector_.snapshot();
    EXPECT_TRUE(snapshot.agent_infos.empty());
    EXPECT_TRUE(snapshot.stat_streams.empty());
}

TEST_F(CollectorOutageAtStartupIntegrationTest,
       ServesNoopSpansDuringOutageAndEnablesTracingAfterRecovery) {
    // The agent must keep retrying registration against the failing
    // collector without ever coming online.
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return results_for(snapshot, CollectorRpc::AgentInfo).size() >= 3;
    }, kWaitTimeout));
    EXPECT_FALSE(agent_->Enable());

    // The application's own work proceeds normally; the disabled agent hands
    // the shared noop span to every request.
    for (int request = 0; request < 5; ++request) {
        auto span = agent_->NewSpan("startup.outage", "/startup-outage");
        ASSERT_NO_FATAL_FAILURE(expect_noop_span(span));
        EXPECT_EQ(handle_instrumented_request(*agent_, "/startup-outage",
                                              request),
                  request * 2 + 1);
    }

    // Nothing but the rejected registration attempts may have reached the
    // collector: no downstream worker starts before AgentInfo is accepted.
    {
        const auto snapshot = collector_.snapshot();
        const auto attempts = results_for(snapshot, CollectorRpc::AgentInfo);
        ASSERT_GE(attempts.size(), 3U);
        for (const auto& result : attempts) {
            EXPECT_EQ(result.status_code, grpc::StatusCode::UNAVAILABLE);
        }
        EXPECT_TRUE(all_span_messages(snapshot).empty());
        EXPECT_TRUE(snapshot.ping_streams.empty());
        EXPECT_TRUE(snapshot.stat_streams.empty());
        EXPECT_TRUE(snapshot.command_streams_v2.empty());
    }

    // Collector recovers: the ongoing registration retry loop must succeed
    // and enable the agent.
    collector_.EndOutage();
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return has_result(snapshot, CollectorRpc::AgentInfo,
                          grpc::StatusCode::OK, true);
    }, std::chrono::seconds(20)));
    ASSERT_TRUE(wait_until([this] { return agent_->Enable(); },
                           std::chrono::seconds(20)));

    // Tracing now runs for real: a fresh span is sampled, carries a trace
    // id, and reaches the collector alongside the ping stream.
    auto recovered = agent_->NewSpan("startup.outage.recovered",
                                     "/startup-outage-recovered");
    EXPECT_NE(recovered, noopSpan());
    ASSERT_TRUE(recovered->IsSampled());
    EXPECT_FALSE(recovered->GetTraceId().empty());
    recovered->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return find_span_by_rpc(snapshot,
                                "/startup-outage-recovered").has_value() &&
               !snapshot.pings.empty();
    }, std::chrono::seconds(20)));
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(AgentIntegrationTest,
       KeepsServingAndRecyclingQueuesThroughCollectorOutage) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    // Healthy baseline: tracing and the stat stream are live.
    {
        auto span = agent_->NewSpan("outage.before", "/collector-outage-before");
        ASSERT_TRUE(span->IsSampled());
        span->EndSpan();
    }
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return find_span_by_rpc(snapshot,
                                "/collector-outage-before").has_value() &&
               !snapshot.stats.empty();
    }, kWaitTimeout));

    // Every RPC now fails while the connections stay up — an unhealthy
    // collector rather than a dead host.
    collector_.BeginOutage();

    // The application-facing side must be unaffected: the agent stays
    // enabled, spans are real (not noop) and requests complete promptly.
    auto probe = agent_->NewSpan("outage.probe", "/collector-outage-probe");
    EXPECT_NE(probe, noopSpan());
    EXPECT_TRUE(probe->IsSampled());
    probe->EndSpan();

    const auto load_started = std::chrono::steady_clock::now();
    for (int request = 0; request < 12; ++request) {
        EXPECT_EQ(handle_instrumented_request(
                      *agent_, "/collector-outage-during", request),
                  request * 2 + 1);
        std::this_thread::sleep_for(25ms);
    }
    EXPECT_LT(std::chrono::steady_clock::now() - load_started,
              std::chrono::seconds(5));
    EXPECT_TRUE(agent_->Enable());

    // The span sender keeps draining its queue into failing batches while
    // recycling its in-flight permits — a permit leak would stall the
    // pipeline after SpanBatch.MaxConcurrentRequests (2) failures.
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        const auto results = results_for(snapshot, CollectorRpc::SendSpanBatch);
        return std::count_if(results.begin(), results.end(),
                             [](const auto& result) {
                                 return result.status_code ==
                                        grpc::StatusCode::UNAVAILABLE;
                             }) >= 3;
    }, kWaitTimeout));

    // The stat stream broke with the outage and the worker keeps reopening
    // it against the failing collector.
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return has_result(snapshot, CollectorRpc::SendAgentStat,
                          grpc::StatusCode::UNAVAILABLE, false);
    }, kWaitTimeout));

    // Metadata queued during the outage fails but is retried on a schedule
    // instead of being dropped; its cache entry (and id) must survive.
    const auto api_id = impl_->cacheApi("collector.outage.api",
                                        API_TYPE_DEFAULT);
    ASSERT_GT(api_id, 0);
    ASSERT_TRUE(collector_.WaitFor([api_id](const auto& snapshot) {
        return std::any_of(snapshot.api_metadata.begin(),
                           snapshot.api_metadata.end(),
            [api_id](const auto& received) {
                return received.message.apiid() == api_id &&
                       received.message.apiinfo() == "collector.outage.api";
            });
    }, kWaitTimeout));

    const auto stats_during_outage = collector_.snapshot().stats.size();
    collector_.EndOutage();

    // The scheduled metadata retry must deliver the same id after recovery:
    // one failed and at least one accepted publication of that id.
    ASSERT_TRUE(collector_.WaitFor([api_id](const auto& snapshot) {
        return std::count_if(snapshot.api_metadata.begin(),
                             snapshot.api_metadata.end(),
            [api_id](const auto& received) {
                return received.message.apiid() == api_id &&
                       received.message.apiinfo() == "collector.outage.api";
            }) >= 2 &&
            has_result(snapshot, CollectorRpc::ApiMetadata,
                       grpc::StatusCode::OK, true);
    }, std::chrono::seconds(20)));

    // Fresh spans flow again and the stat stream re-established itself.
    auto recovered = agent_->NewSpan("outage.after", "/collector-outage-after");
    ASSERT_TRUE(recovered->IsSampled());
    recovered->EndSpan();
    ASSERT_TRUE(collector_.WaitFor([stats_during_outage](const auto& snapshot) {
        return find_span_by_rpc(snapshot,
                                "/collector-outage-after").has_value() &&
               snapshot.stats.size() > stats_during_outage;
    }, std::chrono::seconds(20)));

    // The command stream reconnects too: a collector-originated command must
    // round-trip after recovery.
    collector_.SendEchoCommand(707, "collector-outage-recovered");
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return std::any_of(snapshot.echo_responses.begin(),
                           snapshot.echo_responses.end(),
            [](const auto& response) {
                return response.message.commonresponse().responseid() == 707;
            });
    }, std::chrono::seconds(20)));
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(AgentIntegrationTest,
       HeadDropsOldestSpansWhileSpanEndpointIsDown) {
    // Span.QueueSize 8 keeps the sharded span queue at a single shard (strict
    // FIFO, see ShardedBoundedQueue::compute_shard_count), so the bounded
    // head-drop overflow policy is observable with a handful of spans while
    // the span endpoint is down.
    cfg_.span_queue_size = 8;
    // One worker-owned batch plus the eight-element queue can produce three
    // sends after recovery. Keep permit backpressure out of this queue-policy
    // test so sanitizer callback latency cannot discard the third batch.
    cfg_.span_batch_max_concurrent_requests = 3;
    ASSERT_NO_FATAL_FAILURE(StartStack());

    // Healthy baseline proves the span channel is connected before the
    // outage begins.
    {
        auto span = agent_->NewSpan("queue.before", "/queue-before");
        ASSERT_TRUE(span->IsSampled());
        span->EndSpan();
    }
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/queue-before").has_value();
    }, kWaitTimeout));

    // Connection-level outage on the span endpoint only: the send worker
    // parks in its channel backoff and the bounded queue takes the load.
    ASSERT_TRUE(collector_.StopEndpoint(CollectorEndpoint::Span));
    std::this_thread::sleep_for(300ms);

    constexpr int kOutageSpans = 30;
    const auto load_started = std::chrono::steady_clock::now();
    for (int i = 1; i <= kOutageSpans; ++i) {
        auto span = agent_->NewSpan("queue.outage",
                                    "/queue-outage-" + std::to_string(i));
        EXPECT_TRUE(span->IsSampled()) << i;
        span->EndSpan();
    }
    // The bounded queue absorbs the burst without ever blocking the
    // application on the dead collector.
    EXPECT_LT(std::chrono::steady_clock::now() - load_started,
              std::chrono::seconds(2));
    EXPECT_TRUE(agent_->Enable());

    ASSERT_TRUE(collector_.StartEndpoint(CollectorEndpoint::Span));

    auto recovered = agent_->NewSpan("queue.recovered", "/queue-recovered");
    ASSERT_TRUE(recovered->IsSampled());
    recovered->EndSpan();

    // After recovery the retained tail of the queue and fresh spans arrive
    // (the worker leaves its reconnect backoff once the channel is ready).
    const bool recovered_tail_arrived = collector_.WaitFor([](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/queue-recovered").has_value() &&
               find_span_by_rpc(snapshot, "/queue-outage-30").has_value();
    }, std::chrono::seconds(20));

    const auto snapshot = collector_.snapshot();
    std::vector<std::string> received_rpcs;
    for (const auto& message : all_span_messages(snapshot)) {
        if (message.has_span() && message.span().has_acceptevent()) {
            received_rpcs.push_back(message.span().acceptevent().rpc());
        }
    }
    ASSERT_TRUE(recovered_tail_arrived)
        << "received span RPCs: " << ::testing::PrintToString(received_rpcs);

    // Overflow policy: at most QueueSize (8) queued spans plus one batch
    // (SpanBatch.Size 4) already held by the worker can survive the outage.
    // The newest span is always among the survivors (head-drop discards the
    // oldest), and everything else was dropped instead of growing the queue.
    size_t survivors = 0;
    for (int i = 1; i <= kOutageSpans; ++i) {
        survivors += count_spans_by_rpc(snapshot,
                                        "/queue-outage-" + std::to_string(i));
    }
    EXPECT_LE(survivors, 12U);
    EXPECT_GE(survivors, 1U);
    EXPECT_EQ(count_spans_by_rpc(snapshot, "/queue-outage-30"), 1U);
    EXPECT_EQ(count_spans_by_rpc(snapshot, "/queue-recovered"), 1U);
}

TEST_F(AgentIntegrationTest, ShutdownStopsTracingAndServesNoopSpansToTheApp) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    {
        auto span = agent_->NewSpan("shutdown.noop.before",
                                    "/shutdown-noop-before");
        ASSERT_TRUE(span->IsSampled());
        span->EndSpan();
    }
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/shutdown-noop-before").has_value();
    }, kWaitTimeout));

    const auto started = std::chrono::steady_clock::now();
    agent_->Shutdown();
    EXPECT_LT(std::chrono::steady_clock::now() - started,
              std::chrono::seconds(8));
    EXPECT_FALSE(agent_->Enable());

    // Every worker has been joined by now, so the message-bearing collector
    // records are final.
    const auto quiesced = collector_.snapshot();

    // The application keeps running against the stopped agent: requests
    // complete normally and every span handed out is the shared noop span.
    for (int request = 0; request < 5; ++request) {
        auto span = agent_->NewSpan("shutdown.noop", "/shutdown-noop-after");
        ASSERT_NO_FATAL_FAILURE(expect_noop_span(span));
        EXPECT_EQ(handle_instrumented_request(*agent_, "/shutdown-noop-after",
                                              request),
                  request * 2 + 1);
    }

    // A second shutdown must be a harmless no-op.
    agent_->Shutdown();
    EXPECT_FALSE(agent_->Enable());

    // Nothing new may reach the collector once the agent stopped.
    std::this_thread::sleep_for(300ms);
    const auto after = collector_.snapshot();
    EXPECT_EQ(all_span_messages(after).size(),
              all_span_messages(quiesced).size());
    EXPECT_EQ(after.stats.size(), quiesced.stats.size());
    EXPECT_EQ(after.pings.size(), quiesced.pings.size());
    EXPECT_EQ(after.agent_infos.size(), quiesced.agent_infos.size());
    EXPECT_EQ(after.api_metadata.size(), quiesced.api_metadata.size());
}

// A host that stops and resumes tracing while it keeps serving must go through
// StartAgent() again: Shutdown() is terminal for an agent instance. These two
// pin both halves of that contract end-to-end — the supported cycle keeps
// working, and the unsupported same-handle restart stays refused rather than
// half-starting an agent.

TEST_F(AgentIntegrationTest, StartAfterShutdownIsRefusedAndKeepsServingNoopSpans) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    agent_->Shutdown();
    ASSERT_FALSE(agent_->Enable());
    const auto quiesced = collector_.snapshot();

    // Restarting the same handle is refused, permanently.
    impl_->Start();
    EXPECT_FALSE(wait_until([this] { return agent_->Enable(); },
                            std::chrono::seconds(2)))
        << "a shut-down agent must not come back online through Start()";

    // The application keeps working; it is simply no longer traced.
    for (int request = 0; request < 3; ++request) {
        auto span = agent_->NewSpan("restart.refused", "/restart-refused");
        ASSERT_NO_FATAL_FAILURE(expect_noop_span(span));
        EXPECT_EQ(handle_instrumented_request(*agent_, "/restart-refused", request),
                  request * 2 + 1);
    }

    // GlobalAgent() was cleared by Shutdown() and the refused Start() must not
    // have re-registered anything with the collector.
    EXPECT_FALSE(agent_->Enable());
    std::this_thread::sleep_for(300ms);
    const auto after = collector_.snapshot();
    EXPECT_EQ(after.agent_infos.size(), quiesced.agent_infos.size());
    EXPECT_EQ(all_span_messages(after).size(), all_span_messages(quiesced).size());
}

TEST_F(AgentIntegrationTest, RecoversTracingAcrossRepeatedCreateStartShutdownCycles) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    constexpr int kCycles = 3;
    for (int cycle = 1; cycle <= kCycles; ++cycle) {
        SCOPED_TRACE("cycle " + std::to_string(cycle));
        const auto rpc = "/restart-cycle-" + std::to_string(cycle);

        // A sampled span opened under the outgoing agent, deliberately held
        // across the whole teardown/rebuild below and ended only afterwards.
        // The span keeps its agent alive (SpanImpl::agent_ref_), so ending it
        // late must stay safe rather than touching a destroyed agent.
        auto straddling = agent_->NewSpan("restart.straddle", "/restart-straddle");
        ASSERT_TRUE(straddling->IsSampled());
        auto* straddling_event = straddling->NewSpanEvent("straddle.work");
        ASSERT_NE(straddling_event, nullptr);

        agent_->Shutdown();
        ASSERT_FALSE(agent_->Enable());

        // Between cycles the application keeps calling into the stale handle.
        // Those spans must be dropped, not delivered under the next agent.
        for (int i = 0; i < 3; ++i) {
            auto stale = agent_->NewSpan("restart.stale", "/restart-stale");
            stale->SetStatusCode(200);
            stale->EndSpan();
        }

        // The supported way to resume: drop the dead handle, start a new agent.
        impl_.reset();
        agent_.reset();
        auto options = agent_options();
        options.args = {"--integration-test"};
        options.libs = {"libintegration.so"};
        ASSERT_TRUE(StartAgent(options)) << "restarting the agent unexpectedly failed";
        agent_ = GlobalAgent();
        impl_ = std::dynamic_pointer_cast<AgentImpl>(agent_);
        ASSERT_NE(impl_, nullptr) << "configuration unexpectedly produced a noop agent";
        ASSERT_TRUE(wait_until([this] { return agent_->Enable(); }))
            << "the agent never came back online";

        // Now finish the straddling span: its agent is shut down and no longer
        // the global one, so this must be inert, not a crash.
        ASSERT_NO_FATAL_FAILURE({
            straddling_event->SetDestination("straddle-backend");
            straddling_event->EndEvent();
            straddling->SetStatusCode(200);
            straddling->EndSpan();
        });
        straddling.reset();

        // Tracing works again on the new agent, end to end.
        auto span = agent_->NewSpan("restart.cycle", rpc);
        ASSERT_TRUE(span->IsSampled());
        span->SetStatusCode(200);
        span->EndSpan();
        ASSERT_TRUE(collector_.WaitFor([&rpc](const auto& snapshot) {
            return find_span_by_rpc(snapshot, rpc).has_value();
        }, kWaitTimeout)) << "span never reached the collector";
    }

    const auto snapshot = collector_.snapshot();
    EXPECT_EQ(count_spans_by_rpc(snapshot, "/restart-stale"), 0U)
        << "spans recorded through a shut-down agent must be dropped";
    EXPECT_EQ(count_spans_by_rpc(snapshot, "/restart-straddle"), 0U)
        << "a span ended after its agent shut down must be dropped, "
           "never re-attributed to the replacement agent";
    // One registration for the fixture's agent plus one per rebuilt agent.
    EXPECT_GE(snapshot.agent_infos.size(), static_cast<size_t>(kCycles) + 1U);
}

TEST_F(AgentIntegrationTest, ReloadsConfigAndAppliesNewFilters) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    const auto infos_before = collector_.snapshot().agent_infos.size();
    ASSERT_GE(infos_before, 1U);
    {
        auto probe = agent_->NewSpan("reload.probe", "/reloaded/before");
        EXPECT_TRUE(probe->IsSampled());
        probe->EndSpan();
    }

    // Rebuild the config from the updated sources and reload the live agent —
    // the same path the config-file watcher drives. A repeated StartAgent()
    // must NOT be a reload path: it leaves the running instance untouched.
    cfg_.server_exclude_urls = "[/excluded/**, /reloaded/**]";
    auto reload_cfg = make_config(agent_options(), impl_->getConfig());
    ASSERT_NE(reload_cfg, nullptr);
    impl_->reloadConfig(reload_cfg);
    EXPECT_TRUE(StartAgent(agent_options()));
    EXPECT_EQ(std::dynamic_pointer_cast<AgentImpl>(GlobalAgent()), impl_);

    // The reloaded URL filter must reject new spans while everything else
    // keeps tracing.
    EXPECT_FALSE(agent_->NewSpan("reload.probe", "/reloaded/after")->IsSampled());
    auto sampled = agent_->NewSpan("reload.probe", "/sampled-after-reload");
    ASSERT_TRUE(sampled->IsSampled());
    sampled->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/sampled-after-reload").has_value();
    }, kWaitTimeout));
    const auto snapshot = collector_.snapshot();
    EXPECT_EQ(count_spans_by_rpc(snapshot, "/reloaded/before"), 1U);
    EXPECT_EQ(count_spans_by_rpc(snapshot, "/reloaded/after"), 0U);
    // A config reload must not re-send AgentInfo: the registration count
    // observed before the reload is still the total after spans flowed.
    EXPECT_EQ(snapshot.agent_infos.size(), infos_before);
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(AgentIntegrationTest, RecordsUrlStatsForUnsampledSpan) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    MapCarrier inbound;
    inbound.Set(HEADER_SAMPLED, "s0");
    auto span = agent_->NewSpan("unsampled.server", "/unsampled-propagation", inbound);
    EXPECT_FALSE(span->IsSampled());
    span->SetUrlStat("/unsampled/{id}", "GET", 200);
    span->EndSpan();

    ASSERT_TRUE(FlushUrlStatsUntil("GET /unsampled/{id}", 1));
    const auto totals = uri_stat_totals(collector_.snapshot(),
                                        "GET /unsampled/{id}");
    EXPECT_EQ(totals.total_count, 1);
    EXPECT_EQ(totals.failed_count, 0);
}

TEST_F(AgentIntegrationTest, KeepsTraceContextWhenEventLimitsOverflow) {
    // Uses the smallest limits Config accepts (depth >= 2, sequence >= 4) so
    // the overflow paths are reachable with a handful of events. MaxEventDepth
    // allows max + 1 nesting levels, so depth 2 means three real levels.
    cfg_.max_event_depth = 2;
    cfg_.max_event_sequence = 4;
    ASSERT_NO_FATAL_FAILURE(StartStack());
    ASSERT_EQ(impl_->getConfig()->span.max_event_depth, 2);
    ASSERT_EQ(impl_->getConfig()->span.max_event_sequence, 4);

    auto span = agent_->NewSpan("overflow.depth", "/overflow-depth");
    ASSERT_TRUE(span->IsSampled());
    const auto trace_id = span->GetTraceId();
    const auto span_id = span->GetSpanId();

    auto* real_event = span->NewSpanEvent("depth.level1");
    ASSERT_NE(real_event, nullptr);
    real_event->SetDestination("depth-destination");
    auto* nested_event = span->NewSpanEvent("depth.level2");
    ASSERT_NE(nested_event, nullptr);
    auto* deepest_event = span->NewSpanEvent("depth.level3");
    ASSERT_NE(deepest_event, nullptr);

    // MaxEventDepth allows max + 1 levels (Java DefaultCallStack parity), so
    // depth 1..3 are recorded and only this fourth nesting level overflows
    // into the shared disabled event that records nothing.
    auto* overflowed = span->NewSpanEvent("depth.level4.discarded");
    ASSERT_NE(overflowed, nullptr);
    overflowed->SetDestination("discarded-destination");
    EXPECT_EQ(span->GetSpanEvent(), overflowed);
    EXPECT_FALSE(span->NewAsyncSpan("overflow.async")->IsSampled());

    // A depth overflow is a profiling limit, not a sampling decision: the
    // discarded event still propagates the complete trace context so the
    // distributed trace is not cut here.
    MapCarrier outbound;
    overflowed->InjectContext(outbound);
    EXPECT_EQ(outbound.Get(HEADER_TRACE_ID).value_or(""), trace_id);
    EXPECT_EQ(outbound.Get(HEADER_PARENT_SPAN_ID).value_or(""),
              std::to_string(span_id));
    EXPECT_EQ(outbound.Get(HEADER_HOST).value_or(""), "discarded-destination");
    ASSERT_TRUE(outbound.Get(HEADER_SPAN_ID).has_value());

    auto continued = agent_->NewSpan("overflow.continued",
                                     "/overflow-continued", outbound);
    ASSERT_TRUE(continued->IsSampled());
    EXPECT_EQ(continued->GetTraceId(), trace_id);
    continued->EndSpan();

    // Ending the overflowed placeholder must not desync the event stack.
    overflowed->EndEvent();
    deepest_event->EndEvent();
    nested_event->EndEvent();
    real_event->EndEvent();
    span->EndSpan();

    // MaxEventSequence is 4: a fifth event on one span is discarded even
    // when the depth stays flat.
    auto sequence_span = agent_->NewSpan("overflow.sequence", "/overflow-sequence");
    ASSERT_TRUE(sequence_span->IsSampled());
    const auto sequence_span_id = sequence_span->GetSpanId();
    for (int i = 0; i < 4; ++i) {
        auto* event = sequence_span->NewSpanEvent("seq.event." + std::to_string(i));
        ASSERT_NE(event, nullptr);
        event->EndEvent();
    }
    auto* beyond = sequence_span->NewSpanEvent("seq.event.discarded");
    ASSERT_NE(beyond, nullptr);
    beyond->EndEvent();
    sequence_span->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([span_id, sequence_span_id](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/overflow-depth").has_value() &&
               find_span_by_rpc(snapshot, "/overflow-continued").has_value() &&
               find_span_by_rpc(snapshot, "/overflow-sequence").has_value() &&
               events_for_span(snapshot, span_id).size() >= 3 &&
               events_for_span(snapshot, sequence_span_id).size() >= 4;
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto depth_events = events_for_span(snapshot, span_id);
    ASSERT_EQ(depth_events.size(), 3U)
        << "depth 1..3 are recorded; only the fourth nesting level is dropped";
    // Found by content, not by index: with EventChunkSize=2 these three events
    // arrive split across a SpanChunk and the final PSpan, so their order in
    // the flattened list is a property of the chunking, not of this test.
    const auto with_destination = std::find_if(
        depth_events.begin(), depth_events.end(),
        [](const auto& event) { return event.has_nextevent(); });
    ASSERT_NE(with_destination, depth_events.end())
        << "the recorded event that set a destination is missing";
    EXPECT_EQ(with_destination->nextevent().messageevent().destinationid(),
              "depth-destination");

    EXPECT_EQ(events_for_span(snapshot, sequence_span_id).size(), 4U);
}

TEST_F(AgentIntegrationTest, StartsNewTraceOnUnusableInboundContextAndAcceptsForeignContext) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    // Inbound contexts this agent cannot attach to. Each one starts its own
    // transaction end to end: recorded, with a locally generated trace id and
    // no parent — never dropped, and never a non-root span in a trace whose
    // parent is missing.
    const std::array<std::string_view, 6> unusable{
        "missing-separators",
        "agent-only^123",
        "agent<script>^123^7",
        "agent^ 123 ^7",
        "agent^123^123456789012345678901",
        "",
    };
    for (size_t i = 0; i < unusable.size(); ++i) {
        MapCarrier carrier;
        // Both id headers present: what makes these unusable is the trace id
        // itself, not a missing header (that case is below).
        carrier.Set(HEADER_TRACE_ID, unusable[i]);
        carrier.Set(HEADER_SPAN_ID, "77777");
        carrier.Set(HEADER_PARENT_SPAN_ID, "88888");
        auto span = agent_->NewSpan("unusable.context",
                                    "/unusable/" + std::to_string(i), carrier);
        EXPECT_TRUE(span->IsSampled()) << unusable[i];
        EXPECT_FALSE(span->GetTraceId().empty()) << unusable[i];
        EXPECT_NE(span->GetTraceId(), unusable[i]) << unusable[i];
        EXPECT_NE(span->GetSpanId(), 77777) << unusable[i];
        span->EndSpan();
    }

    // A valid trace id with neither id header is the header-stripping proxy
    // case: also a new transaction, since there is no hop to attach to.
    MapCarrier partial;
    partial.Set(HEADER_TRACE_ID, "java-agent-7^1700000000000^7");
    auto partial_span = agent_->NewSpan("partial.context", "/partial-context",
                                        partial);
    ASSERT_TRUE(partial_span->IsSampled());
    EXPECT_NE(partial_span->GetTraceId(), "java-agent-7^1700000000000^7")
        << "a trace id alone must not be adopted";
    partial_span->EndSpan();

    // A well-formed context from a foreign agent is adopted verbatim, and
    // every parent-describing header must reach the wire.
    MapCarrier carrier;
    carrier.Set(HEADER_TRACE_ID, "java-agent-7^1700000000000^42");
    carrier.Set(HEADER_SPAN_ID, "77777");
    carrier.Set(HEADER_PARENT_SPAN_ID, "88888");
    carrier.Set(HEADER_PARENT_APP_NAME, "upstream-app");
    carrier.Set(HEADER_PARENT_APP_TYPE, "1010");
    carrier.Set(HEADER_PARENT_SERVICE_NAME, "upstream-svc");
    carrier.Set(HEADER_HOST, "gateway.example.test");
    carrier.Set(HEADER_FLAG, "1");
    auto continued = agent_->NewSpan("foreign.continued", "/foreign-continued",
                                     carrier);
    ASSERT_TRUE(continued->IsSampled());
    EXPECT_EQ(continued->GetTraceId(), "java-agent-7^1700000000000^42");
    EXPECT_EQ(continued->GetSpanId(), 77777);
    continued->EndSpan();

    // Every span above is its own transaction now, so wait for all of them —
    // the send queue is sharded and does not guarantee that the last one to
    // arrive is the last one enqueued.
    ASSERT_TRUE(collector_.WaitFor([&](const auto& snapshot) {
        for (size_t i = 0; i < unusable.size(); ++i) {
            if (!find_span_by_rpc(snapshot, "/unusable/" + std::to_string(i))) {
                return false;
            }
        }
        return find_span_by_rpc(snapshot, "/partial-context").has_value() &&
               find_span_by_rpc(snapshot, "/foreign-continued").has_value();
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    for (size_t i = 0; i < unusable.size(); ++i) {
        const auto rpc = "/unusable/" + std::to_string(i);
        EXPECT_EQ(count_spans_by_rpc(snapshot, rpc), 1U) << unusable[i];
        const auto recorded = find_span_by_rpc(snapshot, rpc);
        ASSERT_TRUE(recorded.has_value()) << unusable[i];
        EXPECT_EQ(recorded->transactionid().agentid(), impl_->getAgentId()) << unusable[i];
        EXPECT_EQ(recorded->parentspanid(), -1)
            << "a new transaction has no parent: " << unusable[i];
    }
    const auto partial_wire = find_span_by_rpc(snapshot, "/partial-context");
    ASSERT_TRUE(partial_wire.has_value());
    EXPECT_EQ(partial_wire->transactionid().agentid(), impl_->getAgentId());
    EXPECT_EQ(partial_wire->parentspanid(), -1);
    const auto wire = find_span_by_rpc(snapshot, "/foreign-continued");
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->transactionid().agentid(), "java-agent-7");
    EXPECT_EQ(wire->transactionid().agentstarttime(), INT64_C(1700000000000));
    EXPECT_EQ(wire->transactionid().sequence(), 42);
    EXPECT_EQ(wire->spanid(), 77777);
    EXPECT_EQ(wire->parentspanid(), 88888);
    EXPECT_EQ(wire->flag(), 1);
    EXPECT_EQ(wire->acceptevent().endpoint(), "gateway.example.test");
    EXPECT_EQ(wire->acceptevent().remoteaddr(), "gateway.example.test");
    ASSERT_TRUE(wire->acceptevent().has_parentinfo());
    const auto& parent_info = wire->acceptevent().parentinfo();
    EXPECT_EQ(parent_info.parentapplicationname(), "upstream-app");
    EXPECT_EQ(parent_info.parentapplicationtype(), 1010);
    EXPECT_EQ(parent_info.acceptorhost(), "gateway.example.test");
    EXPECT_EQ(parent_info.parentservicename(), "upstream-svc");
}

TEST_F(AgentIntegrationTest, NormalizesSqlIntoSharedUidMetadata) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    constexpr std::string_view raw_sql =
        "SELECT * FROM orders WHERE id = 42 AND status = 'ready'";
    constexpr std::string_view normalized_sql =
        "SELECT * FROM orders WHERE id = 0# AND status = '1$'";

    auto span = agent_->NewSpan("sql.uid", "/sql-uid");
    ASSERT_TRUE(span->IsSampled());
    const auto span_id = span->GetSpanId();

    auto* first = span->NewSpanEvent("sql.first", SERVICE_TYPE_MYSQL_QUERY);
    ASSERT_NE(first, nullptr);
    first->SetSqlQuery(raw_sql, {});
    first->EndEvent();

    auto* second = span->NewSpanEvent("sql.second", SERVICE_TYPE_MYSQL_QUERY);
    ASSERT_NE(second, nullptr);
    second->SetSqlQuery(raw_sql, {});
    second->EndEvent();
    span->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([span_id, normalized_sql](const auto& snapshot) {
        return events_for_span(snapshot, span_id).size() >= 2 &&
               std::any_of(snapshot.sql_uid_metadata.begin(),
                           snapshot.sql_uid_metadata.end(),
                           [normalized_sql](const auto& received) {
                               return received.message.sql() == normalized_sql;
                           });
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto metadata = std::find_if(
        snapshot.sql_uid_metadata.begin(), snapshot.sql_uid_metadata.end(),
        [normalized_sql](const auto& received) {
            return received.message.sql() == normalized_sql;
        });
    ASSERT_NE(metadata, snapshot.sql_uid_metadata.end());

    const auto events = events_for_span(snapshot, span_id);
    ASSERT_EQ(events.size(), 2U);
    for (const auto& event : events) {
        const auto* uid_annotation = find_annotation(event.annotation(),
                                                     ANNOTATION_SQL_UID);
        ASSERT_NE(uid_annotation, nullptr);
        const auto& value = uid_annotation->value().bytesstringstringvalue();
        EXPECT_EQ(value.bytesvalue(), metadata->message.sqluid());
        EXPECT_EQ(value.stringvalue1().value(), "42,ready");
        EXPECT_TRUE(value.stringvalue2().value().empty());
        EXPECT_EQ(find_annotation(event.annotation(), ANNOTATION_SQL_ID), nullptr);
    }
}

TEST_F(AgentIntegrationTest, SerializesEveryTypedSqlBindValueOnTheWire) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    constexpr std::string_view sql =
        "INSERT INTO typed_values VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    const std::vector<SqlBindValue> bind_values{
        nullptr,
        std::string_view("alpha"),
        true,
        false,
        int32_t{-7},
        uint32_t{8},
        INT64_C(-9000000000),
        UINT64_C(10000000000),
        1.5F,
        2.25,
    };

    auto span = agent_->NewSpan("sql.typed.binds", "/sql-typed-binds");
    ASSERT_TRUE(span->IsSampled());
    const auto span_id = span->GetSpanId();
    auto* event = span->NewSpanEvent("sql.typed.insert",
                                     SERVICE_TYPE_PGSQL_QUERY);
    ASSERT_NE(event, nullptr);
    event->SetSqlQuery(sql, bind_values);
    event->EndEvent();
    span->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([span_id, sql](const auto& snapshot) {
        return !events_for_span(snapshot, span_id).empty() &&
               std::any_of(snapshot.sql_uid_metadata.begin(),
                           snapshot.sql_uid_metadata.end(),
                           [sql](const auto& received) {
                               return received.message.sql() == sql;
                           });
    }, kWaitTimeout));

    const auto events = events_for_span(collector_.snapshot(), span_id);
    ASSERT_EQ(events.size(), 1U);
    const auto* annotation = find_annotation(events[0].annotation(),
                                             ANNOTATION_SQL_UID);
    ASSERT_NE(annotation, nullptr);
    const auto& value = annotation->value().bytesstringstringvalue();
    EXPECT_TRUE(value.stringvalue1().value().empty());
    EXPECT_EQ(value.stringvalue2().value(),
              "null, alpha, true, false, -7, 8, -9000000000, 10000000000, "
              "1.5, 2.25");
}

TEST_F(AgentIntegrationTest,
       OmitsSensitiveSqlBindValuesFromSpanPayload) {
    cfg_.trace_bind_value = false;
    ASSERT_NO_FATAL_FAILURE(StartStack());

    constexpr std::string_view sql =
        "SELECT * FROM secrets WHERE token = ? AND tenant = ?";
    constexpr std::string_view secret = "do-not-collect-this-token";

    auto span = agent_->NewSpan("sql.binds.disabled", "/sql-binds-disabled");
    ASSERT_TRUE(span->IsSampled());
    const auto span_id = span->GetSpanId();
    auto* event = span->NewSpanEvent("sql.secret.lookup",
                                     SERVICE_TYPE_MYSQL_QUERY);
    ASSERT_NE(event, nullptr);
    event->SetSqlQuery(sql, {secret, int32_t{42}});
    event->EndEvent();
    span->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([span_id](const auto& snapshot) {
        return !events_for_span(snapshot, span_id).empty();
    }, kWaitTimeout));

    const auto events = events_for_span(collector_.snapshot(), span_id);
    ASSERT_EQ(events.size(), 1U);
    const auto* annotation = find_annotation(events[0].annotation(),
                                             ANNOTATION_SQL_UID);
    ASSERT_NE(annotation, nullptr);
    const auto& value = annotation->value().bytesstringstringvalue();
    EXPECT_TRUE(value.stringvalue1().value().empty());
    EXPECT_TRUE(value.stringvalue2().value().empty());
    EXPECT_EQ(events[0].SerializeAsString().find(secret), std::string::npos);
}

TEST_F(AgentIntegrationTest, RegistersSqlIdMetadataWhenSqlStatsDisabled) {
    cfg_.enable_sql_stats = false;
    ASSERT_NO_FATAL_FAILURE(StartStack());

    constexpr std::string_view raw_sql =
        "UPDATE inventory SET count = 7 WHERE sku = 'ABC-1'";
    constexpr std::string_view normalized_sql =
        "UPDATE inventory SET count = 0# WHERE sku = '1$'";

    auto span = agent_->NewSpan("sql.id", "/sql-id");
    ASSERT_TRUE(span->IsSampled());
    const auto span_id = span->GetSpanId();

    auto* event = span->NewSpanEvent("sql.update", SERVICE_TYPE_PGSQL_QUERY);
    ASSERT_NE(event, nullptr);
    event->SetSqlQuery(raw_sql, {});
    event->EndEvent();
    span->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([span_id, normalized_sql](const auto& snapshot) {
        return !events_for_span(snapshot, span_id).empty() &&
               std::any_of(snapshot.sql_metadata.begin(),
                           snapshot.sql_metadata.end(),
                           [normalized_sql](const auto& received) {
                               return received.message.sql() == normalized_sql;
                           });
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto metadata = std::find_if(
        snapshot.sql_metadata.begin(), snapshot.sql_metadata.end(),
        [normalized_sql](const auto& received) {
            return received.message.sql() == normalized_sql;
        });
    ASSERT_NE(metadata, snapshot.sql_metadata.end());
    EXPECT_GT(metadata->message.sqlid(), 0);

    const auto events = events_for_span(snapshot, span_id);
    ASSERT_EQ(events.size(), 1U);
    const auto* sql_annotation = find_annotation(events[0].annotation(),
                                                 ANNOTATION_SQL_ID);
    ASSERT_NE(sql_annotation, nullptr);
    const auto& value = sql_annotation->value().intstringstringvalue();
    EXPECT_EQ(value.intvalue(), metadata->message.sqlid());
    EXPECT_EQ(value.stringvalue1().value(), "7,ABC-1");
    EXPECT_TRUE(value.stringvalue2().value().empty());
    EXPECT_EQ(find_annotation(events[0].annotation(), ANNOTATION_SQL_UID), nullptr);

    // SQL-id mode never registers UID metadata.
    EXPECT_TRUE(std::none_of(snapshot.sql_uid_metadata.begin(),
                             snapshot.sql_uid_metadata.end(),
                             [normalized_sql](const auto& received) {
                                 return received.message.sql() == normalized_sql;
                             }));
}

TEST_F(AgentIntegrationTest, AppliesPercentSamplingPattern) {
    cfg_.sampling_type = "PERCENT";
    cfg_.sampling_percent_rate = 50.0;
    ASSERT_NO_FATAL_FAILURE(StartStack());

    const auto baseline = agent_stat_count(collector_.snapshot());

    // PercentSampler accumulates rate (50% == 5000/10000) per request and admits
    // on a remainder in (0, rate], so admission alternates deterministically
    // starting with the first request: sample, skip, sample, skip.
    const std::array<bool, 4> expected{true, false, true, false};
    DriveSamplingPattern("sampling.percent", "/sampling/percent/", expected);

    ASSERT_TRUE(collector_.WaitFor([baseline](const auto& snapshot) {
        const auto totals = transaction_totals_after(snapshot, baseline);
        return totals.sampled_new >= 2 && totals.unsampled_new >= 2;
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto totals = transaction_totals_after(snapshot, baseline);
    EXPECT_EQ(totals.sampled_new, 2);
    EXPECT_EQ(totals.unsampled_new, 2);
    EXPECT_EQ(totals.skipped_new, 0);
    ExpectSamplingPattern(snapshot, "/sampling/percent/", expected);
}

TEST_F(AgentIntegrationTest, SharesAsyncIdAcrossAsyncSpansFromOneEvent) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    auto span = agent_->NewSpan("async.parent", "/async-parent");
    ASSERT_TRUE(span->IsSampled());
    const auto span_id = span->GetSpanId();
    const auto trace_id = span->GetTraceId();

    auto* spawner = span->NewSpanEvent("async.spawner");
    ASSERT_NE(spawner, nullptr);

    // Every async span forked from the same event shares its async id and
    // takes the next sequence number; all of them stay on the parent trace.
    auto first = span->NewAsyncSpan("async.worker.first");
    auto second = span->NewAsyncSpan("async.worker.second");
    ASSERT_TRUE(first->IsSampled());
    ASSERT_TRUE(second->IsSampled());
    EXPECT_EQ(first->GetTraceId(), trace_id);
    EXPECT_EQ(second->GetTraceId(), trace_id);
    EXPECT_EQ(first->GetSpanId(), span_id);
    EXPECT_EQ(second->GetSpanId(), span_id);

    first->EndSpan();
    second->EndSpan();
    spawner->EndEvent();
    span->EndSpan();

    // Span chunks and API metadata use independent workers, so wait for both
    // pipelines before taking the snapshot used below.
    ASSERT_TRUE(collector_.WaitFor([span_id](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/async-parent").has_value() &&
               async_chunks_for(snapshot, span_id).size() >= 2 &&
               has_api_metadata(snapshot, "async.worker.first",
                                API_TYPE_INVOCATION);
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto chunks = async_chunks_for(snapshot, span_id);
    ASSERT_EQ(chunks.size(), 2U);
    EXPECT_EQ(chunks[0].localasyncid().asyncid(),
              chunks[1].localasyncid().asyncid());
    std::vector<int32_t> sequences{chunks[0].localasyncid().sequence(),
                                   chunks[1].localasyncid().sequence()};
    std::sort(sequences.begin(), sequences.end());
    EXPECT_EQ(sequences, (std::vector<int32_t>{1, 2}));
    const auto async_id = chunks[0].localasyncid().asyncid();

    // Each async chunk starts with the async root event.
    for (const auto& chunk : chunks) {
        ASSERT_GE(chunk.spanevent_size(), 1);
        EXPECT_EQ(chunk.spanevent(0).servicetype(), SERVICE_TYPE_ASYNC);
    }

    // The spawning event carries the async id so the collector can stitch the
    // async chunks under it.
    const auto events = events_for_span(snapshot, span_id);
    EXPECT_TRUE(std::any_of(events.begin(), events.end(),
        [async_id](const auto& event) {
            return event.servicetype() != SERVICE_TYPE_ASYNC &&
                   event.asyncevent() == async_id;
        }));
}

TEST_F(AgentIntegrationTest, RestartsActiveThreadCountStreamForDuplicateRequest) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return !snapshot.command_streams_v2.empty();
    }, kWaitTimeout));

    // Every new stream emits sequence 1 immediately.
    collector_.SendActiveThreadCountCommand(501);
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return count_active_thread_responses(snapshot, 501, 1) >= 1;
    }, kWaitTimeout));

    // Re-issuing the same request id (collector reconnect behavior) must
    // replace the old stream: a fresh stream starts over at sequence 1.
    collector_.SendActiveThreadCountCommand(501);
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return count_active_thread_responses(snapshot, 501, 1) >= 2;
    }, kWaitTimeout));
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(AgentIntegrationTest, ParsesApacheProxyHeaderAndRealIpFallback) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    auto span = agent_->NewSpan("http.proxy.apache", "/proxy-apache");
    ASSERT_TRUE(span->IsSampled());

    MapCarrier request;
    request.Set("X-Real-Ip", "203.0.113.99");
    request.Set("Pinpoint-ProxyApache", "t=1710000001000000 D=250 i=7 b=12");
    helper::TraceHttpServerRequest(span, "192.0.2.1:8080",
                                   "apache.example.test:80", request);
    span->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/proxy-apache").has_value();
    }, kWaitTimeout));

    const auto wire = find_span_by_rpc(collector_.snapshot(), "/proxy-apache");
    ASSERT_TRUE(wire.has_value());
    // X-Real-Ip wins over the socket address when no X-Forwarded-For exists.
    EXPECT_EQ(wire->acceptevent().remoteaddr(), "203.0.113.99");
    EXPECT_EQ(wire->acceptevent().endpoint(), "apache.example.test:80");

    const auto* proxy = find_annotation(wire->annotation(),
                                        ANNOTATION_HTTP_PROXY_HEADER);
    ASSERT_NE(proxy, nullptr);
    const auto& value = proxy->value().longintintbytebytestringvalue();
    // Apache reports microseconds; the agent converts to milliseconds and
    // tags the annotation with code 3 plus duration/idle/busy.
    EXPECT_EQ(value.longvalue(), INT64_C(1710000001000));
    EXPECT_EQ(value.intvalue1(), 3);
    EXPECT_EQ(value.intvalue2(), 250);
    EXPECT_EQ(value.bytevalue1(), 7);
    EXPECT_EQ(value.bytevalue2(), 12);
}

TEST_F(AgentIntegrationTest, TracesCompleteSpanThroughCApi) {
    cfg_.server_record_request_headers = "[HEADERS-ALL]";
    ASSERT_NO_FATAL_FAILURE(StartStack());

    // Exercise the standalone lifecycle entry point against the already
    // installed singleton: pt_start_agent() in a process whose agent runs
    // reports success and leaves that same running agent installed.
    EXPECT_NE(pt_start_agent(NULL), 0);

    pt_agent_t agent = pt_global_agent();
    ASSERT_NE(agent, nullptr);
    EXPECT_NE(pt_agent_is_enabled(agent), 0);

    pt_span_t span = pt_agent_new_span_with_method(agent, "c.api.server",
                                                   "/c-api", "GET", nullptr);
    ASSERT_NE(span, nullptr);
    ASSERT_NE(pt_span_is_sampled(span), 0);
    char trace_id[PT_TRACE_ID_MAX];
    ASSERT_GT(pt_span_get_trace_id(span, trace_id, sizeof trace_id), 0U);
    const int64_t span_id = pt_span_get_span_id(span);
    ASSERT_NE(span_id, 0);
    pt_span_set_remote_address(span, "198.51.100.5");
    pt_span_set_acceptor_host(span, "c-api-root.example.test:8080");
    pt_span_set_status_code(span, 200);
    pt_span_set_url_stat(span, "/c-api/{id}", "GET", 200);

    c_api::HeaderMap request_headers{
        {"x-c-api-first", "first-value"},
        {"x-c-api-second", "second-value"},
    };
    pt_header_reader_t request_header_reader{
        &request_headers, c_api::map_get, c_api::map_for_each};
    pt_span_record_header(span, PT_HTTP_REQUEST, &request_header_reader);

    pt_span_event_t event = pt_span_new_event_with_type(
        span, "c.api.client", PT_SERVICE_TYPE_CPP_HTTP_CLIENT);
    ASSERT_NE(event, nullptr);
    pt_span_event_set_destination(event, "c-api-backend");
    pt_span_event_set_end_point(event, "backend.example.test:8080");

    c_api::HeaderMap outbound;
    pt_context_writer_t writer{&outbound, c_api::map_set};
    pt_span_event_inject_context(event, &writer);
    const auto trace_header = outbound.find(std::string(HEADER_TRACE_ID));
    ASSERT_NE(trace_header, outbound.end());
    EXPECT_EQ(trace_header->second, trace_id);
    const auto parent_span_header = outbound.find(
        std::string(HEADER_PARENT_SPAN_ID));
    ASSERT_NE(parent_span_header, outbound.end());
    EXPECT_EQ(parent_span_header->second, std::to_string(span_id));

    c_api::TwoFrameCallStack callstack_frames{{
        {"c_orders", "c_load_order", "orders.c", 42},
        {"c_database", "c_execute", "database.c", 7},
    }};
    pt_callstack_reader_t callstack{&callstack_frames,
                                    c_api::emit_two_frame_callstack};
    pt_span_event_set_error_with_callstack(event, "CApiError", "c call failed",
                                           &callstack);
    pt_span_event_end(event);

    // Continue the trace downstream through the C reader carrier.
    pt_context_reader_t reader{&outbound, c_api::map_get};
    pt_span_t continued = pt_agent_new_span_with_reader(
        agent, "c.api.continued", "/c-api-continued", &reader);
    ASSERT_NE(continued, nullptr);
    EXPECT_NE(pt_span_is_sampled(continued), 0);
    pt_span_set_acceptor_host(continued, "c-api.acceptor.example.test:8080");
    pt_span_end(continued);
    pt_span_destroy(continued);

    pt_span_end(span);
    pt_span_destroy(span);
    pt_agent_destroy(agent);

    // Destroying an already-destroyed handle must be a warn-and-ignore no-op;
    // the token registry protects against double free from C callers.
    pt_span_destroy(span);
    pt_agent_destroy(agent);

    ASSERT_TRUE(collector_.WaitFor([span_id](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/c-api").has_value() &&
               find_span_by_rpc(snapshot, "/c-api-continued").has_value() &&
               !events_for_span(snapshot, span_id).empty() &&
               !snapshot.exception_metadata.empty();
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto wire = find_span_by_rpc(snapshot, "/c-api");
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->spanid(), span_id);
    EXPECT_EQ(wire->acceptevent().remoteaddr(), "198.51.100.5");
    const auto* status = find_annotation(wire->annotation(),
                                         ANNOTATION_HTTP_STATUS_CODE);
    ASSERT_NE(status, nullptr);
    EXPECT_EQ(status->value().intvalue(), 200);
    EXPECT_TRUE(has_string_pair_annotation(
        wire->annotation(), ANNOTATION_HTTP_REQUEST_HEADER,
        "x-c-api-first", "first-value"));
    EXPECT_TRUE(has_string_pair_annotation(
        wire->annotation(), ANNOTATION_HTTP_REQUEST_HEADER,
        "x-c-api-second", "second-value"));

    const auto continued_wire = find_span_by_rpc(snapshot, "/c-api-continued");
    ASSERT_TRUE(continued_wire.has_value());
    ASSERT_TRUE(continued_wire->acceptevent().has_parentinfo());
    EXPECT_EQ(continued_wire->acceptevent().parentinfo().acceptorhost(),
              "c-api.acceptor.example.test:8080");

    const auto events = events_for_span(snapshot, span_id);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].servicetype(), SERVICE_TYPE_CPP_HTTP_CLIENT);
    ASSERT_TRUE(events[0].has_nextevent());
    EXPECT_EQ(events[0].nextevent().messageevent().destinationid(),
              "c-api-backend");
    ASSERT_TRUE(events[0].has_exceptioninfo());
    EXPECT_EQ(events[0].exceptioninfo().stringvalue().value(), "c call failed");

    // The C callstack reader feeds the exception metadata frame by frame.
    const auto exception = std::find_if(
        snapshot.exception_metadata.begin(), snapshot.exception_metadata.end(),
        [span_id](const auto& received) {
            return received.message.spanid() == span_id;
        });
    ASSERT_NE(exception, snapshot.exception_metadata.end());
    EXPECT_EQ(exception->message.uritemplate(), "/c-api/{id}");
    ASSERT_EQ(exception->message.exceptions_size(), 1);
    const auto& frames = exception->message.exceptions(0);
    EXPECT_EQ(frames.exceptionmessage(), "c call failed");
    ASSERT_EQ(frames.stacktraceelement_size(), 2);
    EXPECT_EQ(frames.stacktraceelement(0).classname(), "c_orders");
    EXPECT_EQ(frames.stacktraceelement(0).methodname(), "c_load_order");
    EXPECT_EQ(frames.stacktraceelement(1).filename(), "database.c");
    EXPECT_EQ(frames.stacktraceelement(1).linenumber(), 7);

    ASSERT_TRUE(FlushUrlStatsUntil("GET /c-api/{id}", 1));
    EXPECT_EQ(uri_stat_totals(collector_.snapshot(), "GET /c-api/{id}").total_count, 1);
}

TEST_F(AgentIntegrationTest, FlushesExceptionMetadataForAsyncSpans) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    auto span = agent_->NewSpan("async.exception.parent", "/async-exception");
    ASSERT_TRUE(span->IsSampled());
    const auto span_id = span->GetSpanId();

    // NewAsyncSpan attaches the async id to the currently open span event.
    auto* spawner = span->NewSpanEvent("async.exception.spawner");
    ASSERT_NE(spawner, nullptr);
    auto async = span->NewAsyncSpan("async.exception.worker");
    ASSERT_TRUE(async->IsSampled());
    auto* async_event = async->GetSpanEvent();
    ASSERT_NE(async_event, nullptr);

    // The exception is captured on the async span itself, whose EndSpan runs
    // the async branch: it must flush exception metadata even though the
    // non-async statistics path is skipped there.
    TestCallStack call_stack;
    call_stack.Add("worker", "run_job", "worker.cpp", 21);
    async_event->SetError("AsyncJobError", "async job failed", call_stack);
    async->EndSpan();
    spawner->EndEvent();
    span->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([span_id](const auto& snapshot) {
        return !async_chunks_for(snapshot, span_id).empty() &&
               std::any_of(snapshot.exception_metadata.begin(),
                           snapshot.exception_metadata.end(),
                   [span_id](const auto& received) {
                       return received.message.spanid() == span_id;
                   });
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto chunks = async_chunks_for(snapshot, span_id);
    ASSERT_EQ(chunks.size(), 1U);
    ASSERT_GE(chunks[0].spanevent_size(), 1);
    const auto& async_root = chunks[0].spanevent(0);
    ASSERT_TRUE(async_root.has_exceptioninfo());
    EXPECT_EQ(async_root.exceptioninfo().stringvalue().value(), "async job failed");
    EXPECT_NE(find_annotation(async_root.annotation(), ANNOTATION_EXCEPTION_ID),
              nullptr);

    const auto exception = std::find_if(
        snapshot.exception_metadata.begin(), snapshot.exception_metadata.end(),
        [span_id](const auto& received) {
            return received.message.spanid() == span_id;
        });
    ASSERT_NE(exception, snapshot.exception_metadata.end());
    // Async spans never carry a URL stat, so the template is the literal
    // fallback value.
    EXPECT_EQ(exception->message.uritemplate(), "NULL");
    ASSERT_EQ(exception->message.exceptions_size(), 1);
    EXPECT_EQ(exception->message.exceptions(0).exceptionclassname(), "AsyncJobError")
        << "The class name is the SetError name, not the top frame's module";
    EXPECT_EQ(exception->message.exceptions(0).exceptiondepth(), 0);
    EXPECT_EQ(exception->message.exceptions(0).exceptionmessage(),
              "async job failed");
    ASSERT_EQ(exception->message.exceptions(0).stacktraceelement_size(), 1);
    EXPECT_EQ(exception->message.exceptions(0).stacktraceelement(0).methodname(),
              "run_job");
}

TEST_F(AgentIntegrationTest, RecordsAppProxyHeaderAndGuardsNginxTimestampRange) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    auto app_span = agent_->NewSpan("http.proxy.app", "/proxy-app");
    ASSERT_TRUE(app_span->IsSampled());
    MapCarrier app_request;
    app_request.Set("Pinpoint-ProxyApp", "t=1712345678123 app=edge-proxy");
    helper::TraceHttpServerRequest(app_span, "192.0.2.30:9000",
                                   "app.example.test:80", app_request);
    app_span->EndSpan();

    // 1e300 * 1000 does not fit into int64: the untrusted timestamp must be
    // rejected before the cast, while the annotation itself is still recorded.
    auto nginx_span = agent_->NewSpan("http.proxy.nginx.range",
                                      "/proxy-nginx-range");
    ASSERT_TRUE(nginx_span->IsSampled());
    MapCarrier nginx_request;
    nginx_request.Set("Pinpoint-ProxyNginx", "t=1e300 D=25");
    helper::TraceHttpServerRequest(nginx_span, "192.0.2.31:9000",
                                   "nginx.example.test:80", nginx_request);
    nginx_span->EndSpan();

    // When several proxy headers are present, Apache wins over Nginx and App.
    auto priority_span = agent_->NewSpan("http.proxy.priority", "/proxy-priority");
    ASSERT_TRUE(priority_span->IsSampled());
    MapCarrier priority_request;
    priority_request.Set("Pinpoint-ProxyApache", "t=1710000002000000 D=9");
    priority_request.Set("Pinpoint-ProxyNginx", "t=1710000003.5");
    priority_request.Set("Pinpoint-ProxyApp", "t=1710000004000 app=ignored");
    helper::TraceHttpServerRequest(priority_span, "192.0.2.32:9000",
                                   "priority.example.test:80", priority_request);
    priority_span->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/proxy-app").has_value() &&
               find_span_by_rpc(snapshot, "/proxy-nginx-range").has_value() &&
               find_span_by_rpc(snapshot, "/proxy-priority").has_value();
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto app_wire = find_span_by_rpc(snapshot, "/proxy-app");
    ASSERT_TRUE(app_wire.has_value());
    const auto* app_proxy = find_annotation(app_wire->annotation(),
                                            ANNOTATION_HTTP_PROXY_HEADER);
    ASSERT_NE(app_proxy, nullptr);
    const auto& app_value = app_proxy->value().longintintbytebytestringvalue();
    EXPECT_EQ(app_value.longvalue(), INT64_C(1712345678123));
    EXPECT_EQ(app_value.intvalue1(), 1);
    EXPECT_EQ(app_value.stringvalue().value(), "edge-proxy");

    const auto nginx_wire = find_span_by_rpc(snapshot, "/proxy-nginx-range");
    ASSERT_TRUE(nginx_wire.has_value());
    const auto* nginx_proxy = find_annotation(nginx_wire->annotation(),
                                              ANNOTATION_HTTP_PROXY_HEADER);
    ASSERT_NE(nginx_proxy, nullptr);
    const auto& nginx_value = nginx_proxy->value().longintintbytebytestringvalue();
    EXPECT_EQ(nginx_value.longvalue(), 0);
    EXPECT_EQ(nginx_value.intvalue1(), 2);
    EXPECT_EQ(nginx_value.intvalue2(), 25);

    const auto priority_wire = find_span_by_rpc(snapshot, "/proxy-priority");
    ASSERT_TRUE(priority_wire.has_value());
    const auto* priority_proxy = find_annotation(priority_wire->annotation(),
                                                 ANNOTATION_HTTP_PROXY_HEADER);
    ASSERT_NE(priority_proxy, nullptr);
    const auto& priority_value =
        priority_proxy->value().longintintbytebytestringvalue();
    EXPECT_EQ(priority_value.longvalue(), INT64_C(1710000002000));
    EXPECT_EQ(priority_value.intvalue1(), 3);
    EXPECT_EQ(priority_value.intvalue2(), 9);
}

TEST_F(AgentIntegrationTest, ReRegistersMetadataAfterRetryExhaustion) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    // One initial attempt plus METADATA_RETRY_MAX_ATTEMPTS (3) retries: all
    // four must fail before the sender gives up on this metadata.
    for (int i = 0; i < 4; ++i) {
        collector_.FailNext(CollectorRpc::ApiMetadata,
                            grpc::StatusCode::UNAVAILABLE,
                            "metadata attempt " + std::to_string(i) + " rejected");
    }
    const auto first_id = impl_->cacheApi("retry.exhausted.api", API_TYPE_DEFAULT);
    ASSERT_GT(first_id, 0);

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return results_for(snapshot, CollectorRpc::ApiMetadata).size() >= 4;
    }, kWaitTimeout));
    const auto failed = results_for(collector_.snapshot(),
                                    CollectorRpc::ApiMetadata);
    ASSERT_GE(failed.size(), 4U);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(failed[i].status_code, grpc::StatusCode::UNAVAILABLE) << i;
    }

    // Exhaustion must release the cache entry so the same API string is
    // re-cached under a fresh id and re-published. The release runs on the
    // sender worker shortly after the last failure, hence the poll.
    int32_t second_id = 0;
    ASSERT_TRUE(wait_until([&] {
        second_id = impl_->cacheApi("retry.exhausted.api", API_TYPE_DEFAULT);
        return second_id != first_id;
    }));
    ASSERT_TRUE(collector_.WaitFor([second_id](const auto& snapshot) {
        return std::any_of(snapshot.api_metadata.begin(),
                           snapshot.api_metadata.end(),
                   [second_id](const auto& received) {
                       return received.message.apiinfo() == "retry.exhausted.api" &&
                              received.message.apiid() == second_id;
                   }) &&
               has_result(snapshot, CollectorRpc::ApiMetadata,
                          grpc::StatusCode::OK, true);
    }, kWaitTimeout));
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(AgentIntegrationTest, TruncatesSqlBindArgsAtConfiguredLimit) {
    // Small enough that a handful of short bind values overflows the join
    // limit.
    cfg_.max_bind_args_size = 20;
    ASSERT_NO_FATAL_FAILURE(StartStack());

    auto span = agent_->NewSpan("sql.bind.limit", "/sql-bind-limit");
    ASSERT_TRUE(span->IsSampled());
    const auto span_id = span->GetSpanId();

    auto* event = span->NewSpanEvent("sql.bind", SERVICE_TYPE_MYSQL_QUERY);
    ASSERT_NE(event, nullptr);
    event->SetSqlQuery("SELECT * FROM items WHERE a = ? AND b = ? AND c = ?",
                       {std::string_view{"0123456789"},
                        std::string_view{"abcdefgh"},
                        std::string_view{"xyz"}});
    event->EndEvent();
    span->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([span_id](const auto& snapshot) {
        return !events_for_span(snapshot, span_id).empty();
    }, kWaitTimeout));

    const auto events = events_for_span(collector_.snapshot(), span_id);
    ASSERT_EQ(events.size(), 1U);
    const auto* uid_annotation = find_annotation(events[0].annotation(),
                                                 ANNOTATION_SQL_UID);
    ASSERT_NE(uid_annotation, nullptr);
    // "0123456789, abcdefgh" fills exactly the 20 allowed bytes; the third
    // value no longer fits, so the join stops and — like Java's
    // BindValueUtils — reports how many bind values there were after the
    // separator it had already appended.
    EXPECT_EQ(uid_annotation->value().bytesstringstringvalue()
                  .stringvalue2().value(),
              "0123456789, abcdefgh, ...(3)");
}

TEST_F(AgentIntegrationTest,
       SamplesOnlyContinuedTracesWhenCounterRateIsZero) {
    // CounterRate 0 means "never sample a new trace"; continued traces bypass
    // the base sampler entirely, so they must still be recorded.
    cfg_.sampling_counter_rate = 0;
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return agent_stat_count(snapshot) >= 1;
    }, kWaitTimeout));
    const auto baseline = agent_stat_count(collector_.snapshot());

    const std::array<bool, 3> expected_new{false, false, false};
    DriveSamplingPattern("sampling.zero", "/sampling/zero/", expected_new);

    // Continued traces bypass the new-trace sampler entirely: even a rate
    // that never samples locally must not cut a distributed trace.
    MapCarrier context;
    context.Set(HEADER_TRACE_ID, "java-agent-7^1700000000000^99");
    context.Set(HEADER_SPAN_ID, "77777");
    context.Set(HEADER_PARENT_SPAN_ID, "88888");
    auto continued = agent_->NewSpan("sampling.zero.continued",
                                     "/sampling/zero/continued", context);
    EXPECT_TRUE(continued->IsSampled());
    continued->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([baseline](const auto& snapshot) {
        const auto totals = transaction_totals_after(snapshot, baseline);
        return totals.unsampled_new >= 3 && totals.sampled_continuation >= 1 &&
               count_spans_by_rpc(snapshot, "/sampling/zero/continued") == 1;
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto totals = transaction_totals_after(snapshot, baseline);
    EXPECT_EQ(totals.sampled_new, 0);
    EXPECT_EQ(totals.unsampled_new, 3);
    EXPECT_EQ(totals.sampled_continuation, 1);
    ExpectSamplingPattern(snapshot, "/sampling/zero/", expected_new);
    const auto wire = find_span_by_rpc(snapshot, "/sampling/zero/continued");
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->transactionid().agentid(), "java-agent-7");
}

TEST_F(AgentIntegrationTest, PropagatesUnsampledDecisionDownstream) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return agent_stat_count(snapshot) >= 1;
    }, kWaitTimeout));
    const auto baseline = agent_stat_count(collector_.snapshot());

    MapCarrier inbound;
    inbound.Set(HEADER_SAMPLED, "s0");
    auto span = agent_->NewSpan("unsampled.origin", "/unsampled-origin", inbound);
    EXPECT_FALSE(span->IsSampled());
    EXPECT_TRUE(span->GetTraceId().empty());
    // Unlike a plain noop span, an unsampled span keeps a real span id so it
    // still feeds active-request and response-time statistics.
    EXPECT_NE(span->GetSpanId(), 0);

    // The outbound carrier must tell downstream services to skip sampling,
    // and must not leak any trace identifiers for the untraced request.
    auto* event = span->NewSpanEvent("unsampled.client");
    ASSERT_NE(event, nullptr);
    MapCarrier outbound;
    event->InjectContext(outbound);
    EXPECT_EQ(outbound.Get(HEADER_SAMPLED).value_or(""), "s0");
    EXPECT_FALSE(outbound.Get(HEADER_TRACE_ID).has_value());
    EXPECT_FALSE(outbound.Get(HEADER_SPAN_ID).has_value());
    event->EndEvent();
    span->EndSpan();

    auto downstream = agent_->NewSpan("unsampled.downstream",
                                      "/unsampled-downstream", outbound);
    EXPECT_FALSE(downstream->IsSampled());
    downstream->EndSpan();

    ASSERT_TRUE(collector_.WaitFor([baseline](const auto& snapshot) {
        return transaction_totals_after(snapshot, baseline)
                   .unsampled_continuation >= 2;
    }, kWaitTimeout));
    const auto snapshot = collector_.snapshot();
    EXPECT_EQ(count_spans_by_rpc(snapshot, "/unsampled-origin"), 0U);
    EXPECT_EQ(count_spans_by_rpc(snapshot, "/unsampled-downstream"), 0U);
    const auto totals = transaction_totals_after(snapshot, baseline);
    EXPECT_EQ(totals.unsampled_continuation, 2);
    EXPECT_EQ(totals.sampled_new, 0);
}

TEST_F(AgentIntegrationTest, SendsNoAgentStatsWhenDisabled) {
    cfg_.stat_enable = false;
    ASSERT_NO_FATAL_FAILURE(StartStack());

    // Tracing must be unaffected by the disabled statistics worker.
    auto span = agent_->NewSpan("stat.disabled", "/stat-disabled");
    ASSERT_TRUE(span->IsSampled());
    span->EndSpan();
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/stat-disabled").has_value();
    }, kWaitTimeout));

    // Two full batch intervals are enough for an enabled worker to have
    // shipped at least one agent-stat batch; the disabled worker exits before
    // its collection loop, so nothing may arrive.
    std::this_thread::sleep_for(2500ms);
    EXPECT_EQ(agent_stat_count(collector_.snapshot()), 0U);
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(AgentIntegrationTest, RejectsActiveThreadCountStreamsBeyondLimit) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return !snapshot.command_streams_v2.empty();
    }, kWaitTimeout));

    constexpr int32_t kFirstId = 601;
    constexpr int kMaxStreams = 10;
    for (int i = 0; i < kMaxStreams; ++i) {
        collector_.SendActiveThreadCountCommand(kFirstId + i);
    }
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        for (int i = 0; i < kMaxStreams; ++i) {
            if (count_active_thread_responses(snapshot, kFirstId + i) < 1) {
                return false;
            }
        }
        return true;
    }, kWaitTimeout));

    // The eleventh concurrent stream request must be refused with a fail
    // message instead of silently starting another responder thread.
    constexpr int32_t kRejectedId = kFirstId + kMaxStreams;
    collector_.SendActiveThreadCountCommand(kRejectedId);
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return std::any_of(snapshot.command_stream_messages.begin(),
                           snapshot.command_stream_messages.end(),
            [](const auto& response) {
                return response.message.has_failmessage() &&
                       response.message.failmessage().responseid() == kRejectedId;
            });
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto failure = std::find_if(snapshot.command_stream_messages.begin(),
                                      snapshot.command_stream_messages.end(),
        [](const auto& response) {
            return response.message.has_failmessage() &&
                   response.message.failmessage().responseid() == kRejectedId;
        });
    ASSERT_NE(failure, snapshot.command_stream_messages.end());
    EXPECT_EQ(failure->message.failmessage().message().value(),
              "too many active thread count streams");
    EXPECT_EQ(count_active_thread_responses(snapshot, kRejectedId), 0U);
    EXPECT_TRUE(agent_->Enable());
}

// Runs without the fixture: a disabled configuration must fail StartAgent()
// and leave GlobalAgent() serving a noop agent that needs no collector and
// never registers itself as the global agent.
TEST(DisabledAgentIntegrationTest, CreatesNoopAgentWhenDisabledByConfig) {
    AgentOptions options;
    options.env_prefix = "PINPOINT_CPP_AGENT_IT_ISOLATED";
    options.config_yaml = "Enable: false\nApplicationName: noop-agent-it\n";
    options.app_type = kApplicationType;
    options.server_info = "disabled agent";

    EXPECT_FALSE(StartAgent(options)) << "a disabled configuration must not start an agent";
    auto agent = GlobalAgent();
    ASSERT_NE(agent, nullptr);
    EXPECT_EQ(std::dynamic_pointer_cast<AgentImpl>(agent), nullptr);
    EXPECT_FALSE(agent->Enable());

    auto span = agent->NewSpan("noop.operation", "/noop");
    ASSERT_NE(span, nullptr);
    EXPECT_FALSE(span->IsSampled());
    EXPECT_TRUE(span->GetTraceId().empty());
    EXPECT_EQ(span->GetSpanId(), 0);

    auto* event = span->NewSpanEvent("noop.event");
    ASSERT_NE(event, nullptr);
    MapCarrier outbound;
    event->InjectContext(outbound);
    EXPECT_FALSE(outbound.Get(HEADER_TRACE_ID).has_value());
    EXPECT_FALSE(outbound.Get(HEADER_SAMPLED).has_value());
    event->EndEvent();
    span->EndSpan();
    span->EndSpan();

    // The noop agent's lifecycle entry points must be inert and safe.
    EXPECT_FALSE(agent->Enable());
    agent->Shutdown();
}

TEST_F(AgentIntegrationTest, ShutdownCancelsTimedOutSpanRequest) {
    ASSERT_NO_FATAL_FAILURE(StartStack());

    collector_.TimeoutNext(CollectorRpc::SendSpanBatch);
    auto span = agent_->NewSpan("shutdown.timeout", "/timeout-shutdown");
    ASSERT_TRUE(span->IsSampled());
    span->EndSpan();
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return find_span_by_rpc(snapshot, "/timeout-shutdown").has_value();
    }, kWaitTimeout));

    const auto started = std::chrono::steady_clock::now();
    agent_->Shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::seconds(8));
    EXPECT_FALSE(agent_->Enable());
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return has_result(snapshot, CollectorRpc::SendSpanBatch,
                          grpc::StatusCode::DEADLINE_EXCEEDED, false);
    }, 2s));
}

}  // namespace
}  // namespace pinpoint::test
