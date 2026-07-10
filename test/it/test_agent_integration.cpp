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
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "pinpoint/tracer.h"
#include "src/agent.h"
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

bool has_async_chunk(const CollectorSnapshot& snapshot, int64_t span_id) {
    for (const auto& message : all_span_messages(snapshot)) {
        if (message.has_spanchunk() && message.spanchunk().spanid() == span_id &&
            message.spanchunk().has_localasyncid()) {
            return true;
        }
    }
    return false;
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

std::vector<RpcResult> results_for(const CollectorSnapshot& snapshot,
                                   CollectorRpc rpc) {
    std::vector<RpcResult> results;
    std::copy_if(snapshot.rpc_results.begin(), snapshot.rpc_results.end(),
                 std::back_inserter(results), [rpc](const auto& result) {
                     return result.rpc == rpc;
                 });
    return results;
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

void expect_common_metadata(const RpcMetadata& metadata, bool expect_socket_id) {
    EXPECT_EQ(metadata.value("applicationname").value_or(""), "cpp-agent-it");
    EXPECT_EQ(metadata.value("agentid").value_or(""), "cpp-it-agent");
    EXPECT_EQ(metadata.value("agentname").value_or(""), "cpp-it-agent-name");
    EXPECT_EQ(metadata.value("servicetype").value_or(""), std::to_string(kApplicationType));
    EXPECT_EQ(metadata.value("protocol.version").value_or(""), "100");
    EXPECT_FALSE(metadata.value("starttime").value_or("").empty());
    EXPECT_EQ(metadata.value("socketid").has_value(), expect_socket_id);
}

class AgentIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(collector_.Start());
        ASSERT_GT(collector_.agent_port(), 0);
        ASSERT_GT(collector_.span_port(), 0);
        ASSERT_GT(collector_.stat_port(), 0);

        // A test-only prefix prevents a developer's PINPOINT_CPP_* environment
        // from overriding the deterministic inline configuration below.
        SetConfigEnvVarPrefix("PINPOINT_CPP_AGENT_IT_ISOLATED");
        SetConfigFilePath("");
        SetConfigString(config());

        agent_ = CreateAgent(kApplicationType,
                             "mock-collector integration server",
                             {"--integration-test", "--ephemeral-ports"},
                             {"libintegration.so", "libmock-collector.so"});
        impl_ = std::dynamic_pointer_cast<AgentImpl>(agent_);
        ASSERT_NE(impl_, nullptr) << "configuration unexpectedly produced a noop agent";

        ConfigureBeforeAgentStart();
        agent_->Start();
        ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
            return !snapshot.agent_infos.empty();
        }, kWaitTimeout));
        ASSERT_TRUE(wait_until([this] { return agent_->Enable(); }));
    }

    void TearDown() override {
        if (agent_) {
            agent_->Shutdown();
        }
        impl_.reset();
        agent_.reset();
        collector_.Shutdown();
        SetConfigString("");
        SetConfigFilePath("");
        SetConfigEnvVarPrefix("");
    }

    std::string config() const {
        std::ostringstream yaml;
        yaml
            << "Enable: true\n"
            << "ApplicationName: cpp-agent-it\n"
            << "AgentId: cpp-it-agent\n"
            << "AgentName: cpp-it-agent-name\n"
            << "UidVersion: v3\n"
            << "IsContainer: true\n"
            << "EnableCallstackTrace: true\n"
            << "Log:\n"
            << "  Level: error\n"
            << "Collector:\n"
            << "  Host: " << collector_.host() << "\n"
            << "  AgentPort: " << collector_.agent_port() << "\n"
            << "  SpanPort: " << collector_.span_port() << "\n"
            << "  StatPort: " << collector_.stat_port() << "\n"
            << "  AgentInfo:\n"
            << "    RefreshIntervalMs: 60000\n"
            << "    SendRetryIntervalMs: 50\n"
            << "    MaxTryPerAttempt: 2\n"
            << "  SpanBatch:\n"
            << "    Size: 4\n"
            << "    FlushIntervalMs: 50\n"
            << "    CollectDeadlineMs: 20\n"
            << "    MaxConcurrentRequests: 2\n"
            << "Stat:\n"
            << "  Enable: true\n"
            << "  BatchCount: 1\n"
            << "  BatchInterval: 1000\n"
            << "Sampling:\n"
            << "  Type: COUNTER\n"
            << "  CounterRate: 1\n"
            << "Span:\n"
            << "  QueueSize: 128\n"
            << "  MaxEventDepth: 16\n"
            << "  MaxEventSequence: 128\n"
            << "  EventChunkSize: 2\n"
            << "Http:\n"
            << "  CollectUrlStat: true\n"
            << "  UrlStatEnableTrimPath: false\n"
            << "  UrlStatMethodPrefix: true\n"
            << "  Server:\n"
            << "    StatusCodeErrors: [4xx, 5xx]\n"
            << "    ExcludeUrl: [/excluded/**]\n"
            << "    ExcludeMethod: [OPTIONS]\n"
            << "    RecordRequestHeader: [x-request-id]\n"
            << "    RecordRequestCookie: [session_id]\n"
            << "    RecordResponseHeader: [x-response-id]\n"
            << "  Client:\n"
            << "    RecordRequestHeader: [x-client-request]\n"
            << "    RecordRequestCookie: [client_session]\n"
            << "    RecordResponseHeader: [x-client-response]\n"
            << "Sql:\n"
            << "  EnableSqlStats: true\n"
            << "  MaxBindArgsSize: 2048\n";
        return yaml.str();
    }

    virtual void ConfigureBeforeAgentStart() {}

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

TEST_F(AgentIntegrationTest, RegistersAgentAndMaintainsPingAndCommandStreams) {
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

    expect_common_metadata(received_info.metadata, false);
    ASSERT_FALSE(snapshot.ping_streams.empty());
    expect_common_metadata(snapshot.ping_streams.front(), true);
    ASSERT_FALSE(snapshot.command_streams_v2.empty());
    expect_common_metadata(snapshot.command_streams_v2.front(), true);
    EXPECT_EQ(snapshot.command_streams_v2.front()
                  .value("supportcommandcode").value_or(""),
              "710;730");
}

TEST_F(PingFailureIntegrationTest, ReconnectsPingStreamAfterResponseError) {
    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        return snapshot.ping_streams.size() >= 2 &&
               !snapshot.pings.empty() &&
               has_result(snapshot, CollectorRpc::PingSession,
                          grpc::StatusCode::UNAVAILABLE, false);
    }, kWaitTimeout));
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(AgentIntegrationTest, SendsAllMetadataAndCompleteSpanShapes) {
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
    auto* root_annotations = root->GetAnnotations();
    root_annotations->AppendInt(9000, 7);
    root_annotations->AppendLong(9001, INT64_C(9000000000));
    root_annotations->AppendString(9002, "root-value");
    root_annotations->AppendStringString(9003, "left", "right");
    root_annotations->AppendIntStringString(9004, 11, "one", "two");
    root_annotations->AppendSqlUidStringString(9005, annotation_uid, "sql", "args");
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
    outbound->SetSqlQuery("SELECT * FROM orders WHERE id = 42", "42");

    // SQL-stat mode naturally emits the UID form. Exercise the legacy SQL-id
    // metadata RPC too, then attach its id exactly as a non-stat SQL event does.
    const auto sql_id = impl_->cacheSql("SELECT status FROM orders WHERE id=?");
    ASSERT_GT(sql_id, 0);
    outbound->GetAnnotations()->AppendIntStringString(
        ANNOTATION_SQL_ID, sql_id, "42", "bind=42");

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
        event->GetAnnotations()->AppendInt(9100 + i, i);
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
               has_async_chunk(snapshot, root_span_id) &&
               !snapshot.api_metadata.empty() &&
               !snapshot.string_metadata.empty() &&
               !snapshot.sql_metadata.empty() &&
               !snapshot.sql_uid_metadata.empty() &&
               !snapshot.exception_metadata.empty();
    }, kWaitTimeout));

    const auto snapshot = collector_.snapshot();
    const auto root_wire = find_span_by_rpc(snapshot, "/orders/42");
    ASSERT_TRUE(root_wire.has_value());
    EXPECT_EQ(root_wire->spanid(), root_span_id);
    EXPECT_EQ(root_wire->transactionid().agentid(), "cpp-it-agent");
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
    EXPECT_NE(find_annotation(database_event->annotation(), ANNOTATION_SQL_ID), nullptr);
    EXPECT_NE(find_annotation(database_event->annotation(), ANNOTATION_SQL_UID), nullptr);
    EXPECT_NE(find_annotation(database_event->annotation(), ANNOTATION_EXCEPTION_ID), nullptr);
    EXPECT_NE(find_annotation(database_event->annotation(),
                              ANNOTATION_HTTP_REQUEST_HEADER), nullptr);

    ASSERT_FALSE(snapshot.sql_metadata.empty());
    EXPECT_EQ(snapshot.sql_metadata.front().message.sql(),
              "SELECT status FROM orders WHERE id=?");
    ASSERT_FALSE(snapshot.sql_uid_metadata.empty());
    EXPECT_EQ(snapshot.sql_uid_metadata.front().message.sqluid().size(), 16U);
    ASSERT_FALSE(snapshot.exception_metadata.empty());
    const auto& exception = snapshot.exception_metadata.front().message;
    EXPECT_EQ(exception.spanid(), root_span_id);
    EXPECT_EQ(exception.uritemplate(), "/orders/{id}");
    ASSERT_EQ(exception.exceptions_size(), 1);
    EXPECT_EQ(exception.exceptions(0).exceptionclassname(), "orders");
    EXPECT_EQ(exception.exceptions(0).exceptionmessage(), "connection refused");
    ASSERT_EQ(exception.exceptions(0).stacktraceelement_size(), 2);
    EXPECT_EQ(exception.exceptions(0).stacktraceelement(1).methodname(), "execute");

    ASSERT_FALSE(snapshot.span_batches.empty());
    expect_common_metadata(snapshot.span_batches.front().metadata, false);
}

TEST_F(AgentIntegrationTest, StreamsAgentAndUrlStatistics) {
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

    const auto snapshot = collector_.snapshot();
    ASSERT_FALSE(snapshot.stat_streams.empty());
    expect_common_metadata(snapshot.stat_streams.front(), false);

    bool checked_agent_stat = false;
    for (const auto& received : snapshot.stats) {
        if (!received.message.has_agentstatbatch() ||
            received.message.agentstatbatch().agentstat().empty()) {
            continue;
        }
        const auto& stat = received.message.agentstatbatch().agentstat(0);
        EXPECT_GT(stat.timestamp(), 0);
        EXPECT_EQ(stat.collectinterval(), 1000);
        ASSERT_TRUE(stat.has_transaction());
        EXPECT_GE(stat.transaction().samplednewcount(), 1);
        ASSERT_TRUE(stat.has_responsetime());
        ASSERT_TRUE(stat.has_totalthread());
        EXPECT_GT(stat.totalthread().totalthreadcount(), 0);
        checked_agent_stat = true;
        break;
    }
    EXPECT_TRUE(checked_agent_stat);

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

TEST_F(AgentIntegrationTest, HandlesProfilerCommandsOverRealGrpcStreams) {
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
        return std::any_of(snapshot.active_thread_count_responses.begin(),
                           snapshot.active_thread_count_responses.end(),
            [](const auto& response) {
                return response.message.commonstreamresponse().responseid() == 102;
            });
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
    expect_common_metadata(echo->metadata, false);

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
    expect_common_metadata(active_count->metadata, true);

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

TEST_F(AgentIntegrationTest, RetriesMetadataAfterGrpcAndApplicationErrors) {
    collector_.FailNext(CollectorRpc::ApiMetadata,
                        grpc::StatusCode::UNAVAILABLE,
                        "metadata endpoint unavailable");
    collector_.RejectNext(CollectorRpc::ApiMetadata,
                          "collector rejected metadata");

    const auto api_id = impl_->cacheApi("fault.retry.api", API_TYPE_DEFAULT);
    ASSERT_GT(api_id, 0);

    ASSERT_TRUE(collector_.WaitFor([](const auto& snapshot) {
        const auto matching_requests = std::count_if(
            snapshot.api_metadata.begin(), snapshot.api_metadata.end(),
            [](const auto& received) {
                return received.message.apiinfo() == "fault.retry.api";
            });
        return matching_requests >= 3 &&
               results_for(snapshot, CollectorRpc::ApiMetadata).size() >= 3;
    }, kWaitTimeout));

    const auto results = results_for(collector_.snapshot(),
                                     CollectorRpc::ApiMetadata);
    ASSERT_GE(results.size(), 3U);
    EXPECT_EQ(results[0].status_code, grpc::StatusCode::UNAVAILABLE);
    EXPECT_FALSE(results[0].response_success);
    EXPECT_EQ(results[1].status_code, grpc::StatusCode::OK);
    EXPECT_FALSE(results[1].response_success);
    EXPECT_EQ(results[2].status_code, grpc::StatusCode::OK);
    EXPECT_TRUE(results[2].response_success);
    EXPECT_TRUE(agent_->Enable());
}

TEST_F(AgentIntegrationTest, TimesOutCommandRequestAndKeepsStreamUsable) {
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

TEST_F(AgentIntegrationTest, ReconnectsStatStreamAfterServerError) {
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

TEST_F(AgentIntegrationTest, ShutdownCancelsTimedOutSpanRequest) {
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
