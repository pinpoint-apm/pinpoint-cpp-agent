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

/**
 * @file test_tracer_c.cpp
 * @brief Unit tests for the pure-C public API (include/pinpoint/tracer_c.h).
 *
 * This file is compiled as C++ so that GoogleTest and the mock gRPC
 * infrastructure can be used, but it exercises only functions and types
 * declared in tracer_c.h — the same header that C applications include.
 *
 * A mock-backed pinpoint::AgentImpl is installed as the global agent via
 * the internal set_global_agent() helper so that tests run without a real
 * Pinpoint collector.
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

// C API under test — included exactly as a C application would include it.
#include "pinpoint/tracer_c.h"

// Internal headers needed only to set up the mock agent for testing.
#include "../src/agent.h"
#include "../src/config.h"
#include "../src/grpc.h"
#include "../src/noop.h"
#include "v1/Service_mock.grpc.pb.h"

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

// ============================================================================
// Mock gRPC infrastructure (same pattern as test_agent_with_mocks.cpp)
// ============================================================================

namespace {

class TestableGrpcAgent : public pinpoint::GrpcAgent {
public:
    explicit TestableGrpcAgent(std::shared_ptr<const pinpoint::Config> cfg)
        : GrpcAgent(std::move(cfg)) {}

    void injectMockStubs() {
        auto agent_stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
        EXPECT_CALL(*agent_stub, RequestAgentInfo(_, _, _))
            .WillRepeatedly(Return(grpc::Status::OK));
        set_agent_stub(std::move(agent_stub));

        auto meta_stub = std::make_unique<NiceMock<v1::MockMetadataStub>>();
        EXPECT_CALL(*meta_stub, RequestApiMetaData(_, _, _))
            .WillRepeatedly(Return(grpc::Status::OK));
        EXPECT_CALL(*meta_stub, RequestStringMetaData(_, _, _))
            .WillRepeatedly(Return(grpc::Status::OK));
        EXPECT_CALL(*meta_stub, RequestSqlMetaData(_, _, _))
            .WillRepeatedly(Return(grpc::Status::OK));
        EXPECT_CALL(*meta_stub, RequestSqlUidMetaData(_, _, _))
            .WillRepeatedly(Return(grpc::Status::OK));
        set_meta_stub(std::move(meta_stub));
    }

    bool readyChannel() override { return ready_count_++ == 0; }
    pinpoint::GrpcRequestStatus registerAgent() override { return pinpoint::SEND_OK; }

private:
    std::atomic<int> ready_count_{0};
};

class TestableGrpcSpan : public pinpoint::GrpcSpan {
public:
    explicit TestableGrpcSpan(std::shared_ptr<const pinpoint::Config> cfg)
        : GrpcSpan(std::move(cfg)) {}

    void injectMockStubs() {
        set_span_stub(std::make_unique<NiceMock<v1::MockSpanStub>>());
    }

    bool readyChannel() override { return false; }
};

class TestableGrpcStats : public pinpoint::GrpcStats {
public:
    explicit TestableGrpcStats(std::shared_ptr<const pinpoint::Config> cfg)
        : GrpcStats(std::move(cfg)) {}

    void injectMockStubs() {
        set_stats_stub(std::make_unique<NiceMock<v1::MockStatStub>>());
    }

    bool readyChannel() override { return false; }
};

static std::shared_ptr<pinpoint::Config> make_test_config() {
    auto cfg = std::make_shared<pinpoint::Config>();
    cfg->enable              = true;
    cfg->app_name_           = "c-api-test";
    cfg->app_type_           = PT_APP_TYPE_CPP;
    cfg->agent_id_           = "c-api-agent";
    cfg->agent_name_         = "c-api-agent-name";
    cfg->collector.host      = "127.0.0.1";
    cfg->collector.agent_port = 9991;
    cfg->collector.span_port  = 9993;
    cfg->collector.stat_port  = 9992;
    cfg->span.queue_size      = 1024;
    cfg->span.event_chunk_size = 10;
    cfg->span.max_event_depth  = 32;
    cfg->stat.enable           = true;
    cfg->stat.collect_interval = 5000;
    cfg->http.url_stat.enable  = false;
    cfg->sampling.type         = "counter";
    cfg->sampling.counter_rate = 1;
    return cfg;
}

static std::shared_ptr<pinpoint::AgentImpl> make_test_agent(
        std::shared_ptr<pinpoint::Config> cfg) {
    auto grpc_agent = std::make_unique<TestableGrpcAgent>(cfg);
    auto grpc_span  = std::make_unique<TestableGrpcSpan>(cfg);
    auto grpc_stat  = std::make_unique<TestableGrpcStats>(cfg);

    grpc_agent->injectMockStubs();
    grpc_span->injectMockStubs();
    grpc_stat->injectMockStubs();

    return std::make_shared<pinpoint::AgentImpl>(
        cfg,
        std::move(grpc_agent),
        std::move(grpc_span),
        std::move(grpc_stat));
}

// ============================================================================
// C-compatible callback helpers
//
// These are plain functions (no lambda captures) that implement the carrier
// interfaces using std::map<std::string,std::string> as the backing store.
// ============================================================================

using HeaderMap = std::map<std::string, std::string>;

static const char* hmap_get(void* ud, const char* key) {
    auto* m = static_cast<HeaderMap*>(ud);
    auto  it = m->find(key);
    return it != m->end() ? it->second.c_str() : nullptr;
}

static void hmap_set(void* ud, const char* key, const char* value) {
    (*static_cast<HeaderMap*>(ud))[key] = value;
}

static void hmap_foreach(void* ud, pt_header_foreach_cb cb, void* cb_ud) {
    for (const auto& kv : *static_cast<HeaderMap*>(ud)) {
        if (cb(kv.first.c_str(), kv.second.c_str(), cb_ud) != 0) break;
    }
}

// Callstack data for testing.
struct Frame { const char* mod; const char* fn; const char* file; int line; };
static const Frame kTestFrames[] = {
    { "libfoo", "foo::bar()",  "foo.cpp", 42 },
    { "libfoo", "foo::entry()", "foo.cpp", 10 },
};

static void callstack_foreach(void* ud, pt_callstack_frame_cb cb, void* cb_ud) {
    const auto* frames = static_cast<const Frame*>(ud);
    for (int i = 0; i < 2; ++i) {
        cb(frames[i].mod, frames[i].fn, frames[i].file, frames[i].line, cb_ud);
    }
}

// ============================================================================
// Base test fixture — installs a mock agent and exposes a pt_agent_t handle
// ============================================================================

class TracerCApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        pinpoint::reset_global_agent();
        pinpoint::set_config_string("");

        cfg_        = make_test_config();
        mock_agent_ = make_test_agent(cfg_);
        pinpoint::set_global_agent(mock_agent_);
        wait_enabled();

        // Obtain C handle from the global agent.
        agent_ = pt_global_agent();
        ASSERT_NE(agent_, nullptr);
    }

    void TearDown() override {
        pt_agent_destroy(agent_);
        agent_ = nullptr;

        mock_agent_->Shutdown();
        mock_agent_.reset();
        pinpoint::reset_global_agent();
        pinpoint::set_config_string("");
    }

    void wait_enabled(int timeout_ms = 3000) {
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(timeout_ms);
        while (!mock_agent_->Enable()
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::shared_ptr<pinpoint::Config>   cfg_;
    std::shared_ptr<pinpoint::AgentImpl> mock_agent_;
    pt_agent_t                           agent_ = nullptr;
};

}  // namespace

// ============================================================================
// 1. Constant values
// ============================================================================

TEST(TracerCConstantsTest, AnnotationKeys) {
    EXPECT_EQ(PT_ANNOTATION_API,                  12);
    EXPECT_EQ(PT_ANNOTATION_SQL_ID,               20);
    EXPECT_EQ(PT_ANNOTATION_SQL_UID,              25);
    EXPECT_EQ(PT_ANNOTATION_EXCEPTION_ID,         -52);
    EXPECT_EQ(PT_ANNOTATION_HTTP_URL,             40);
    EXPECT_EQ(PT_ANNOTATION_HTTP_STATUS_CODE,     46);
    EXPECT_EQ(PT_ANNOTATION_HTTP_COOKIE,          45);
    EXPECT_EQ(PT_ANNOTATION_HTTP_REQUEST_HEADER,  47);
    EXPECT_EQ(PT_ANNOTATION_HTTP_RESPONSE_HEADER, 55);
    EXPECT_EQ(PT_ANNOTATION_HTTP_PROXY_HEADER,    300);
}

TEST(TracerCConstantsTest, ServiceTypes) {
    EXPECT_EQ(PT_APP_TYPE_CPP,                 1300);
    EXPECT_EQ(PT_SERVICE_TYPE_CPP,             1300);
    EXPECT_EQ(PT_SERVICE_TYPE_CPP_FUNC,        1301);
    EXPECT_EQ(PT_SERVICE_TYPE_CPP_HTTP_CLIENT, 9800);
    EXPECT_EQ(PT_SERVICE_TYPE_ASYNC,           100);
    EXPECT_EQ(PT_SERVICE_TYPE_MYSQL_QUERY,     2101);
    EXPECT_EQ(PT_SERVICE_TYPE_MSSQL_QUERY,     2201);
    EXPECT_EQ(PT_SERVICE_TYPE_ORACLE_QUERY,    2301);
    EXPECT_EQ(PT_SERVICE_TYPE_PGSQL_QUERY,     2501);
    EXPECT_EQ(PT_SERVICE_TYPE_CASSANDRA_QUERY, 2601);
    EXPECT_EQ(PT_SERVICE_TYPE_MONGODB_QUERY,   2651);
    EXPECT_EQ(PT_SERVICE_TYPE_MEMCACHED,       8050);
    EXPECT_EQ(PT_SERVICE_TYPE_REDIS,           8203);
    EXPECT_EQ(PT_SERVICE_TYPE_KAFKA,           8660);
    EXPECT_EQ(PT_SERVICE_TYPE_HBASE,           8800);
    EXPECT_EQ(PT_SERVICE_TYPE_GRPC_CLIENT,     9160);
    EXPECT_EQ(PT_SERVICE_TYPE_GRPC_SERVER,     1130);
}

TEST(TracerCConstantsTest, HeaderNames) {
    EXPECT_STREQ(PT_HEADER_TRACE_ID,             "Pinpoint-TraceID");
    EXPECT_STREQ(PT_HEADER_SPAN_ID,              "Pinpoint-SpanID");
    EXPECT_STREQ(PT_HEADER_PARENT_SPAN_ID,       "Pinpoint-pSpanID");
    EXPECT_STREQ(PT_HEADER_SAMPLED,              "Pinpoint-Sampled");
    EXPECT_STREQ(PT_HEADER_FLAG,                 "Pinpoint-Flags");
    EXPECT_STREQ(PT_HEADER_PARENT_APP_NAME,      "Pinpoint-pAppName");
    EXPECT_STREQ(PT_HEADER_PARENT_APP_TYPE,      "Pinpoint-pAppType");
    EXPECT_STREQ(PT_HEADER_PARENT_APP_NAMESPACE, "Pinpoint-pAppNamespace");
    EXPECT_STREQ(PT_HEADER_HOST,                 "Pinpoint-Host");
}

// ============================================================================
// 2. Agent lifecycle
// ============================================================================

TEST_F(TracerCApiTest, AgentIsEnabled) {
    EXPECT_NE(pt_agent_is_enabled(agent_), 0);
}

TEST_F(TracerCApiTest, AgentShutdownDisablesAgent) {
    EXPECT_NE(pt_agent_is_enabled(agent_), 0);
    pt_agent_shutdown(agent_);
    EXPECT_EQ(pt_agent_is_enabled(agent_), 0);
}

// ============================================================================
// 3. Null-handle safety — no crashes on NULL inputs
// ============================================================================

TEST(TracerCNullSafetyTest, NullAgentCalls) {
    EXPECT_NO_FATAL_FAILURE(pt_agent_is_enabled(nullptr));
    EXPECT_NO_FATAL_FAILURE(pt_agent_shutdown(nullptr));
    EXPECT_NO_FATAL_FAILURE(pt_agent_destroy(nullptr));
    EXPECT_EQ(pt_agent_new_span(nullptr, "op", "/rpc"), nullptr);
    EXPECT_EQ(pt_agent_new_span_with_reader(nullptr, "op", "/rpc", nullptr), nullptr);
    EXPECT_EQ(pt_agent_new_span_with_method(nullptr, "op", "/rpc", "GET", nullptr), nullptr);
}

TEST(TracerCNullSafetyTest, NullSpanCalls) {
    EXPECT_NO_FATAL_FAILURE(pt_span_destroy(nullptr));
    EXPECT_EQ(pt_span_new_event(nullptr, "op"), nullptr);
    EXPECT_EQ(pt_span_new_event_with_type(nullptr, "op", 0), nullptr);
    EXPECT_EQ(pt_span_get_event(nullptr), nullptr);
    EXPECT_NO_FATAL_FAILURE(pt_span_end_event(nullptr));
    EXPECT_NO_FATAL_FAILURE(pt_span_end(nullptr));
    EXPECT_EQ(pt_span_new_async_span(nullptr, "op"), nullptr);
    EXPECT_NO_FATAL_FAILURE(pt_span_inject_context(nullptr, nullptr));
    EXPECT_NO_FATAL_FAILURE(pt_span_extract_context(nullptr, nullptr));
    EXPECT_EQ(pt_span_get_span_id(nullptr), 0);
    EXPECT_EQ(pt_span_is_sampled(nullptr), 0);
    EXPECT_NO_FATAL_FAILURE(pt_span_set_service_type(nullptr, 0));
    EXPECT_NO_FATAL_FAILURE(pt_span_set_start_time_ms(nullptr, 0));
    EXPECT_NO_FATAL_FAILURE(pt_span_set_remote_address(nullptr, "addr"));
    EXPECT_NO_FATAL_FAILURE(pt_span_set_end_point(nullptr, "ep"));
    EXPECT_NO_FATAL_FAILURE(pt_span_set_error(nullptr, "err"));
    EXPECT_NO_FATAL_FAILURE(pt_span_set_error_named(nullptr, "n", "m"));
    EXPECT_NO_FATAL_FAILURE(pt_span_set_status_code(nullptr, 200));
    EXPECT_NO_FATAL_FAILURE(pt_span_set_url_stat(nullptr, "/", "GET", 200));
    EXPECT_NO_FATAL_FAILURE(pt_span_set_logging(nullptr, nullptr));
    EXPECT_NO_FATAL_FAILURE(pt_span_record_header(nullptr, PT_HTTP_REQUEST, nullptr));
    EXPECT_EQ(pt_span_get_annotations(nullptr), nullptr);
}

TEST(TracerCNullSafetyTest, NullSpanEventCalls) {
    EXPECT_NO_FATAL_FAILURE(pt_span_event_destroy(nullptr));
    EXPECT_NO_FATAL_FAILURE(pt_span_event_set_service_type(nullptr, 0));
    EXPECT_NO_FATAL_FAILURE(pt_span_event_set_operation_name(nullptr, "op"));
    EXPECT_NO_FATAL_FAILURE(pt_span_event_set_start_time_ms(nullptr, 0));
    EXPECT_NO_FATAL_FAILURE(pt_span_event_set_destination(nullptr, "dst"));
    EXPECT_NO_FATAL_FAILURE(pt_span_event_set_end_point(nullptr, "ep"));
    EXPECT_NO_FATAL_FAILURE(pt_span_event_set_error(nullptr, "err"));
    EXPECT_NO_FATAL_FAILURE(pt_span_event_set_error_named(nullptr, "n", "m"));
    EXPECT_NO_FATAL_FAILURE(pt_span_event_set_error_with_callstack(nullptr, "n", "m", nullptr));
    EXPECT_NO_FATAL_FAILURE(pt_span_event_set_sql_query(nullptr, "SELECT 1", ""));
    EXPECT_NO_FATAL_FAILURE(pt_span_event_record_header(nullptr, PT_HTTP_REQUEST, nullptr));
    EXPECT_EQ(pt_span_event_get_annotations(nullptr), nullptr);
}

TEST(TracerCNullSafetyTest, NullAnnotationCalls) {
    EXPECT_NO_FATAL_FAILURE(pt_annotation_destroy(nullptr));
    EXPECT_NO_FATAL_FAILURE(pt_annotation_append_int(nullptr, PT_ANNOTATION_API, 0));
    EXPECT_NO_FATAL_FAILURE(pt_annotation_append_long(nullptr, PT_ANNOTATION_API, 0));
    EXPECT_NO_FATAL_FAILURE(pt_annotation_append_string(nullptr, PT_ANNOTATION_API, "s"));
    EXPECT_NO_FATAL_FAILURE(pt_annotation_append_string_string(nullptr, PT_ANNOTATION_API, "a", "b"));
    EXPECT_NO_FATAL_FAILURE(pt_annotation_append_int_string_string(nullptr, PT_ANNOTATION_API, 1, "a", "b"));
    EXPECT_NO_FATAL_FAILURE(pt_annotation_append_bytes_string_string(nullptr, PT_ANNOTATION_API, nullptr, 0, "a", "b"));
    EXPECT_NO_FATAL_FAILURE(pt_annotation_append_long_int_int_byte_byte_string(nullptr, PT_ANNOTATION_API, 0, 0, 0, 0, 0, "s"));
}

// ============================================================================
// 4. Span creation
// ============================================================================

TEST_F(TracerCApiTest, NewSpanReturnsHandle) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);
    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, NewSpanWithReaderExtracts) {
    // Pre-populate a carrier that has no Pinpoint headers — should still
    // succeed and return a valid (root) span.
    HeaderMap headers;
    pt_context_reader_t reader{&headers, hmap_get};

    pt_span_t span = pt_agent_new_span_with_reader(agent_, "op", "/rpc", &reader);
    ASSERT_NE(span, nullptr);
    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, NewSpanWithReaderNullFallsBack) {
    pt_span_t span = pt_agent_new_span_with_reader(agent_, "op", "/rpc", nullptr);
    ASSERT_NE(span, nullptr);
    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, NewSpanWithMethod) {
    HeaderMap headers;
    pt_context_reader_t reader{&headers, hmap_get};

    pt_span_t span = pt_agent_new_span_with_method(agent_, "op", "/rpc", "POST", &reader);
    ASSERT_NE(span, nullptr);
    pt_span_end(span);
    pt_span_destroy(span);
}

// ============================================================================
// 5. Span metadata setters
// ============================================================================

TEST_F(TracerCApiTest, SpanSettersDontCrash) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_span_set_service_type(span, PT_SERVICE_TYPE_CPP);
    pt_span_set_start_time_ms(span, 1700000000000LL);
    pt_span_set_remote_address(span, "10.0.0.1");
    pt_span_set_end_point(span, "myhost:8080");
    pt_span_set_error(span, "something went wrong");
    pt_span_set_error_named(span, "RuntimeError", "index out of bounds");
    pt_span_set_status_code(span, 500);
    pt_span_set_url_stat(span, "/api/v1/users", "GET", 200);

    pt_span_end(span);
    pt_span_destroy(span);
}

// ============================================================================
// 6. Span identifier accessors
// ============================================================================

TEST_F(TracerCApiTest, GetSpanIdReturnsNonZero) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    EXPECT_NE(pt_span_get_span_id(span), 0);
    EXPECT_NE(pt_span_is_sampled(span), 0);

    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, GetTraceIdFillsStruct) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_trace_id_t tid = pt_span_get_trace_id(span);
    EXPECT_STRNE(tid.agent_id, "");
    EXPECT_NE(tid.start_time, 0);

    pt_span_end(span);
    pt_span_destroy(span);
}

// ============================================================================
// 7. Context injection / extraction
// ============================================================================

TEST_F(TracerCApiTest, InjectContextWritesHeaders) {
    // InjectContext uses the active span event to generate the child span ID,
    // so a span event must be open before calling inject.
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_span_event_t se = pt_span_new_event_with_type(span, "http_out",
                                                      PT_SERVICE_TYPE_CPP_HTTP_CLIENT);
    ASSERT_NE(se, nullptr);

    HeaderMap out;
    pt_context_writer_t writer{&out, hmap_set};
    pt_span_inject_context(span, &writer);

    // After injection the trace-id header must be present.
    EXPECT_FALSE(out.empty());
    EXPECT_NE(out.find(PT_HEADER_TRACE_ID), out.end());

    pt_span_end_event(span);
    pt_span_event_destroy(se);
    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, ExtractContextFromInjectedHeaders) {
    // Create a parent span with an active span event and inject its context.
    pt_span_t parent = pt_agent_new_span(agent_, "parent", "/rpc");
    ASSERT_NE(parent, nullptr);

    pt_span_event_t parent_se = pt_span_new_event_with_type(parent, "http_out",
                                                             PT_SERVICE_TYPE_CPP_HTTP_CLIENT);
    ASSERT_NE(parent_se, nullptr);

    HeaderMap carrier;
    pt_context_writer_t writer{&carrier, hmap_set};
    pt_span_inject_context(parent, &writer);
    ASSERT_FALSE(carrier.empty());

    // Create a child span by extracting from the carrier.
    pt_context_reader_t reader{&carrier, hmap_get};
    pt_span_t child = pt_agent_new_span_with_reader(agent_, "child", "/rpc", &reader);
    ASSERT_NE(child, nullptr);

    // Child must share the same trace-id as the parent.
    pt_trace_id_t parent_tid = pt_span_get_trace_id(parent);
    pt_trace_id_t child_tid  = pt_span_get_trace_id(child);
    EXPECT_STREQ(parent_tid.agent_id, child_tid.agent_id);
    EXPECT_EQ(parent_tid.start_time, child_tid.start_time);
    EXPECT_EQ(parent_tid.sequence, child_tid.sequence);

    pt_span_end(child);
    pt_span_destroy(child);
    pt_span_end_event(parent);
    pt_span_event_destroy(parent_se);
    pt_span_end(parent);
    pt_span_destroy(parent);
}

// ============================================================================
// 8. Span event lifecycle
// ============================================================================

TEST_F(TracerCApiTest, SpanEventLifecycle) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_span_event_t se = pt_span_new_event(span, "db_query");
    ASSERT_NE(se, nullptr);

    pt_span_event_set_service_type(se, PT_SERVICE_TYPE_MYSQL_QUERY);
    pt_span_event_set_operation_name(se, "SELECT users");
    pt_span_event_set_destination(se, "mydb");
    pt_span_event_set_end_point(se, "db-host:3306");
    pt_span_event_set_sql_query(se, "SELECT * FROM users WHERE id = ?", "42");

    pt_span_end_event(span);
    pt_span_event_destroy(se);

    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, SpanEventWithType) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_span_event_t se = pt_span_new_event_with_type(span, "http_call",
                                                      PT_SERVICE_TYPE_CPP_HTTP_CLIENT);
    ASSERT_NE(se, nullptr);

    pt_span_end_event(span);
    pt_span_event_destroy(se);

    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, SpanEventSetStartTime) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_span_event_t se = pt_span_new_event(span, "timed_op");
    ASSERT_NE(se, nullptr);

    pt_span_event_set_start_time_ms(se, 1700000000000LL);

    pt_span_end_event(span);
    pt_span_event_destroy(se);
    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, GetSpanEventReturnsHandle) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_span_event_t created = pt_span_new_event(span, "sub_op");
    ASSERT_NE(created, nullptr);

    pt_span_event_t fetched = pt_span_get_event(span);
    ASSERT_NE(fetched, nullptr);

    // Both handles refer to the same underlying event.
    EXPECT_EQ(pt_span_event_get_annotations(created) != nullptr, true);

    pt_span_end_event(span);
    pt_span_event_destroy(fetched);
    pt_span_event_destroy(created);
    pt_span_end(span);
    pt_span_destroy(span);
}

// ============================================================================
// 9. Span event error recording
// ============================================================================

TEST_F(TracerCApiTest, SpanEventSetError) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_span_event_t se = pt_span_new_event(span, "failing_op");
    ASSERT_NE(se, nullptr);

    pt_span_event_set_error(se, "connection refused");
    pt_span_event_set_error_named(se, "IOError", "connection refused");

    pt_callstack_reader_t cs_reader;
    cs_reader.userdata  = const_cast<Frame*>(kTestFrames);
    cs_reader.for_each  = callstack_foreach;
    pt_span_event_set_error_with_callstack(se, "IOError", "connection refused", &cs_reader);

    pt_span_end_event(span);
    pt_span_event_destroy(se);
    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, SpanEventSetErrorWithNullCallstack) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_span_event_t se = pt_span_new_event(span, "failing_op");
    ASSERT_NE(se, nullptr);

    EXPECT_NO_FATAL_FAILURE(
        pt_span_event_set_error_with_callstack(se, "Err", "msg", nullptr));

    pt_span_end_event(span);
    pt_span_event_destroy(se);
    pt_span_end(span);
    pt_span_destroy(span);
}

// ============================================================================
// 10. Annotation operations
// ============================================================================

TEST_F(TracerCApiTest, SpanAnnotationsAllTypes) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_annotation_t anno = pt_span_get_annotations(span);
    ASSERT_NE(anno, nullptr);

    // int
    pt_annotation_append_int(anno, PT_ANNOTATION_HTTP_STATUS_CODE, 200);
    // long
    pt_annotation_append_long(anno, PT_ANNOTATION_API, 99999LL);
    // string
    pt_annotation_append_string(anno, PT_ANNOTATION_HTTP_URL, "http://example.com/api");
    // string+string
    pt_annotation_append_string_string(anno, PT_ANNOTATION_HTTP_REQUEST_HEADER,
                                       "X-Custom", "value");
    // int+string+string
    pt_annotation_append_int_string_string(anno, PT_ANNOTATION_HTTP_PROXY_HEADER,
                                           1, "proxy", "addr");
    // bytes+string+string
    unsigned char uid[] = {0xDE, 0xAD, 0xBE, 0xEF};
    pt_annotation_append_bytes_string_string(anno, PT_ANNOTATION_SQL_UID,
                                             uid, 4,
                                             "SELECT 1", "");
    // long+int+int+byte+byte+string
    pt_annotation_append_long_int_int_byte_byte_string(
        anno, PT_ANNOTATION_HTTP_PROXY_HEADER,
        /*l=*/12345678L, /*i1=*/8080, /*i2=*/0, /*b1=*/1, /*b2=*/0,
        "endpoint");

    pt_annotation_destroy(anno);
    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, SpanEventAnnotationsAllTypes) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_span_event_t se   = pt_span_new_event(span, "event");
    ASSERT_NE(se, nullptr);

    pt_annotation_t anno = pt_span_event_get_annotations(se);
    ASSERT_NE(anno, nullptr);

    pt_annotation_append_int(anno, PT_ANNOTATION_HTTP_STATUS_CODE, 404);
    pt_annotation_append_string(anno, PT_ANNOTATION_HTTP_URL, "http://example.com/");

    pt_annotation_destroy(anno);
    pt_span_end_event(span);
    pt_span_event_destroy(se);
    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, AnnotationDestroyIsSafeToCallMultipleTimes) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_annotation_t a1 = pt_span_get_annotations(span);
    pt_annotation_t a2 = pt_span_get_annotations(span);
    ASSERT_NE(a1, nullptr);
    ASSERT_NE(a2, nullptr);

    pt_annotation_destroy(a1);
    pt_annotation_destroy(a2);

    pt_span_end(span);
    pt_span_destroy(span);
}

// ============================================================================
// 11. Header reader with ForEach
// ============================================================================

TEST_F(TracerCApiTest, SpanRecordHeaderWithForEach) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    HeaderMap req_headers;
    req_headers["Content-Type"]   = "application/json";
    req_headers["X-Request-Id"]   = "abc-123";
    req_headers["Authorization"]  = "Bearer token";

    pt_header_reader_t reader{&req_headers, hmap_get, hmap_foreach};
    pt_span_record_header(span, PT_HTTP_REQUEST, &reader);

    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, SpanEventRecordHeaderWithForEach) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_span_event_t se = pt_span_new_event(span, "http_call");
    ASSERT_NE(se, nullptr);

    HeaderMap resp_headers;
    resp_headers["Content-Type"] = "application/json";
    resp_headers["X-Trace-Id"]   = "trace-xyz";

    pt_header_reader_t reader{&resp_headers, hmap_get, hmap_foreach};
    pt_span_event_record_header(se, PT_HTTP_RESPONSE, &reader);

    pt_span_end_event(span);
    pt_span_event_destroy(se);
    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, ForEachStopsEarlyOnNonZeroReturn) {
    // Verify that the ForEach callback honours the "stop" return value.
    HeaderMap headers;
    headers["A"] = "1";
    headers["B"] = "2";
    headers["C"] = "3";

    struct Ctx { int count; };
    Ctx ctx{0};

    auto stop_after_one = [](const char*, const char*, void* ud) -> int {
        auto* c = static_cast<Ctx*>(ud);
        c->count++;
        return 1;  // stop immediately after the first entry
    };

    hmap_foreach(&headers, stop_after_one, &ctx);
    EXPECT_EQ(ctx.count, 1);
}

// ============================================================================
// 12. Callstack reader callback
// ============================================================================

TEST_F(TracerCApiTest, CallstackReaderIteratesFrames) {
    struct Ctx { std::vector<std::string> fns; };
    Ctx ctx;

    pt_callstack_reader_t cs_reader;
    cs_reader.userdata = const_cast<Frame*>(kTestFrames);
    cs_reader.for_each = callstack_foreach;

    // Drive the iteration manually to verify the callback receives correct data.
    cs_reader.for_each(
        cs_reader.userdata,
        [](const char* mod, const char* fn, const char* file, int line, void* ud) {
            static_cast<Ctx*>(ud)->fns.push_back(fn);
            (void)mod; (void)file; (void)line;
        },
        &ctx);

    ASSERT_EQ(ctx.fns.size(), 2u);
    EXPECT_EQ(ctx.fns[0], "foo::bar()");
    EXPECT_EQ(ctx.fns[1], "foo::entry()");
}

// ============================================================================
// 13. Async span
// ============================================================================

TEST_F(TracerCApiTest, NewAsyncSpan) {
    pt_span_t parent = pt_agent_new_span(agent_, "parent", "/rpc");
    ASSERT_NE(parent, nullptr);

    pt_span_t async_span = pt_span_new_async_span(parent, "bg_task");
    ASSERT_NE(async_span, nullptr);

    pt_span_end(async_span);
    pt_span_destroy(async_span);

    pt_span_end(parent);
    pt_span_destroy(parent);
}

// ============================================================================
// 14. HTTP helper functions
// ============================================================================

TEST_F(TracerCApiTest, TraceHttpServerRequest) {
    pt_span_t span = pt_agent_new_span(agent_, "GET /api", "/api");
    ASSERT_NE(span, nullptr);

    HeaderMap req_headers;
    req_headers["Content-Type"] = "application/json";
    req_headers["User-Agent"]   = "test-client/1.0";

    pt_header_reader_t reader{&req_headers, hmap_get, hmap_foreach};

    EXPECT_NO_FATAL_FAILURE(
        pt_trace_http_server_request(span, "192.168.1.1", "api.example.com", &reader));

    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, TraceHttpServerRequestWithCookie) {
    pt_span_t span = pt_agent_new_span(agent_, "GET /page", "/page");
    ASSERT_NE(span, nullptr);

    HeaderMap req_headers;
    req_headers["Accept"] = "text/html";

    HeaderMap cookies;
    cookies["session"] = "abc123";
    cookies["pref"]    = "dark";

    pt_header_reader_t req_reader{&req_headers, hmap_get, hmap_foreach};
    pt_header_reader_t cookie_reader{&cookies, hmap_get, hmap_foreach};

    EXPECT_NO_FATAL_FAILURE(
        pt_trace_http_server_request_with_cookie(
            span, "10.0.0.1", "www.example.com",
            &req_reader, &cookie_reader));

    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, TraceHttpServerResponse) {
    pt_span_t span = pt_agent_new_span(agent_, "GET /api", "/api");
    ASSERT_NE(span, nullptr);

    HeaderMap resp_headers;
    resp_headers["Content-Type"]   = "application/json";
    resp_headers["Content-Length"] = "128";

    pt_header_reader_t reader{&resp_headers, hmap_get, hmap_foreach};

    EXPECT_NO_FATAL_FAILURE(
        pt_trace_http_server_response(span, "/api/v1/*", "GET", 200, &reader));

    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, TraceHttpClientRequest) {
    pt_span_t span = pt_agent_new_span(agent_, "outer", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_span_event_t se = pt_span_new_event_with_type(span, "http_out",
                                                      PT_SERVICE_TYPE_CPP_HTTP_CLIENT);
    ASSERT_NE(se, nullptr);

    HeaderMap out_headers;
    pt_header_reader_t reader{&out_headers, hmap_get, hmap_foreach};

    EXPECT_NO_FATAL_FAILURE(
        pt_trace_http_client_request(se, "downstream.svc:8080",
                                     "http://downstream.svc:8080/api", &reader));

    pt_span_end_event(span);
    pt_span_event_destroy(se);
    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, TraceHttpClientRequestWithCookie) {
    pt_span_t span = pt_agent_new_span(agent_, "outer", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_span_event_t se = pt_span_new_event_with_type(span, "http_out",
                                                      PT_SERVICE_TYPE_CPP_HTTP_CLIENT);
    ASSERT_NE(se, nullptr);

    HeaderMap req_headers;
    HeaderMap cookie_headers;
    cookie_headers["auth"] = "tok";

    pt_header_reader_t req_reader{&req_headers, hmap_get, hmap_foreach};
    pt_header_reader_t cookie_reader{&cookie_headers, hmap_get, hmap_foreach};

    EXPECT_NO_FATAL_FAILURE(
        pt_trace_http_client_request_with_cookie(
            se, "downstream.svc:8080", "http://downstream.svc:8080/api",
            &req_reader, &cookie_reader));

    pt_span_end_event(span);
    pt_span_event_destroy(se);
    pt_span_end(span);
    pt_span_destroy(span);
}

TEST_F(TracerCApiTest, TraceHttpClientResponse) {
    pt_span_t span = pt_agent_new_span(agent_, "outer", "/rpc");
    ASSERT_NE(span, nullptr);

    pt_span_event_t se = pt_span_new_event_with_type(span, "http_out",
                                                      PT_SERVICE_TYPE_CPP_HTTP_CLIENT);
    ASSERT_NE(se, nullptr);

    HeaderMap resp_headers;
    resp_headers["X-Response-Time"] = "42ms";

    pt_header_reader_t reader{&resp_headers, hmap_get, hmap_foreach};

    EXPECT_NO_FATAL_FAILURE(
        pt_trace_http_client_response(se, 200, &reader));

    pt_span_end_event(span);
    pt_span_event_destroy(se);
    pt_span_end(span);
    pt_span_destroy(span);
}

// ============================================================================
// 15. Logging injection
// ============================================================================

TEST_F(TracerCApiTest, SetLoggingWritesToCarrier) {
    pt_span_t span = pt_agent_new_span(agent_, "op", "/rpc");
    ASSERT_NE(span, nullptr);

    HeaderMap mdc;
    pt_context_writer_t writer{&mdc, hmap_set};
    pt_span_set_logging(span, &writer);

    // At minimum the span ID should have been injected into the MDC map.
    EXPECT_FALSE(mdc.empty());

    pt_span_end(span);
    pt_span_destroy(span);
}
