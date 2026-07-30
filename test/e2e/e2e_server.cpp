#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "pinpoint/tracer.h"
#include "httplib.h"
#include "http_trace_context.h"

#include "e2e_common.h"
#include "pinpoint_grpc_context.h"
#include "pinpoint_grpc_interceptors.h"
#include "testapp.grpc.pb.h"

// =============================================================================
// Global state
// =============================================================================
static std::atomic<uint64_t> total_requests{0};
static std::atomic<uint64_t> active_requests{0};
static auto start_time = std::chrono::steady_clock::now();

static pinpoint::AgentPtr g_agent;
static std::mutex g_agent_mutex;
static std::string g_http_target = "localhost:8091";

// =============================================================================
// Common helpers
// =============================================================================
httplib::Headers parse_cookies(const httplib::Request& req) {
    httplib::Headers cookies;
    const auto raw = req.get_header_value("Cookie");
    size_t start = 0;
    while (start < raw.size()) {
        const auto end = raw.find(';', start);
        const auto item = raw.substr(start, end == std::string::npos
                                               ? std::string::npos
                                               : end - start);
        const auto equals = item.find('=');
        if (equals != std::string::npos) {
            const auto key_start = item.find_first_not_of(" \t");
            if (key_start != std::string::npos && key_start < equals) {
                cookies.emplace(item.substr(key_start, equals - key_start),
                                item.substr(equals + 1));
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return cookies;
}

pinpoint::SpanPtr make_span(const httplib::Request& req) {
    auto agent = pinpoint::GlobalAgent();
    HttpHeaderReader h_reader(req.headers);
    auto span = agent->NewSpan("it-test-server", req.path, req.method, h_reader);

    auto end_point = req.get_header_value("Host");
    if (end_point.empty()) {
        end_point = req.local_addr + ":" + std::to_string(req.local_port);
    }
    auto cookies = parse_cookies(req);
    HttpHeaderReader cookie_reader(cookies);
    pinpoint::helper::TraceHttpServerRequest(
        span, req.remote_addr, end_point, h_reader, cookie_reader);
    return span;
}

void finish_span(const httplib::Request& req, httplib::Response& res,
                 pinpoint::SpanPtr span) {
    res.set_header("X-It-Trace-Id", span->GetTraceId());
    res.set_header("X-It-Span-Id", std::to_string(span->GetSpanId()));
    HttpHeaderReader http_reader(res.headers);
    pinpoint::helper::TraceHttpServerResponse(
        span, req.path, req.method, res.status, http_reader);
    span->EndSpan();
}

// RAII-style request tracker
struct RequestTracker {
    RequestTracker() { ++total_requests; ++active_requests; }
    ~RequestTracker() { --active_requests; }
};

// =============================================================================
// HTTP-only endpoints (no MySQL dependency)
// =============================================================================

// Simple endpoint: minimal span with one event
void on_simple(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);

    span->NewSpanEvent("simple_work")->EndEvent();

    res.status = 200;
    res.set_content("ok", "text/plain");
    finish_span(req, res, span);
}

// Deep nesting: creates deeply nested span events
void on_deep(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);

    int depth = 20;
    auto depth_param = req.get_param_value("depth");
    if (!depth_param.empty()) {
        depth = std::min(std::max(std::stoi(depth_param), 1), 256);
    }

    std::vector<pinpoint::SpanEventPtr> events;
    events.reserve(depth);
    for (int i = 0; i < depth; ++i) {
        events.push_back(span->NewSpanEvent("deep_level_" + std::to_string(i)));
    }
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        (*it)->EndEvent();
    }

    res.status = 200;
    res.set_content("depth=" + std::to_string(depth), "text/plain");
    finish_span(req, res, span);
}

// Wide: many sequential span events
void on_wide(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);

    int width = 50;
    auto width_param = req.get_param_value("width");
    if (!width_param.empty()) {
        width = std::min(std::max(std::stoi(width_param), 1), 10000);
    }

    for (int i = 0; i < width; ++i) {
        span->NewSpanEvent("wide_event_" + std::to_string(i))->EndEvent();
    }

    res.status = 200;
    res.set_content("width=" + std::to_string(width), "text/plain");
    finish_span(req, res, span);
}

// Annotation-heavy: records many annotations on span events
void on_annotated(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);

    for (int i = 0; i < 10; ++i) {
        auto ev = span->NewSpanEvent("annotated_op_" + std::to_string(i));
        if (ev) {
            ev->SetServiceType(pinpoint::SERVICE_TYPE_CPP_FUNC);
            ev->SetDestination("test-dest-" + std::to_string(i));
            ev->SetEndPoint("test-endpoint-" + std::to_string(i));
            auto ann = ev->GetAnnotations();
            if (ann) {
                ann->AppendString(pinpoint::ANNOTATION_HTTP_URL,
                                  "/annotated/" + std::to_string(i));
                ann->AppendInt(pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);
                ann->AppendStringString(pinpoint::ANNOTATION_HTTP_REQUEST_HEADER,
                                        "X-Custom-" + std::to_string(i),
                                        "value-" + std::to_string(i));
            }
        }
        ev->EndEvent();
    }

    res.status = 200;
    res.set_content("annotated", "text/plain");
    finish_span(req, res, span);
}

// Mixed workload: combines deep nesting, annotations, and async spans
void on_mixed(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);

    // Nested span events simulating DB query
    auto ev = span->NewSpanEvent("db_query");
    if (ev) {
        ev->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
        ev->SetEndPoint("localhost:3306");
        ev->SetDestination("test");
        ev->SetSqlQuery("SELECT * FROM users WHERE id = ?", {"42"});
    }
    span->NewSpanEvent("db_parse")->EndEvent();
    ev->EndEvent();

    // Simulate HTTP client call
    ev = span->NewSpanEvent("http_client_call");
    if (ev) {
        ev->SetServiceType(pinpoint::SERVICE_TYPE_CPP_HTTP_CLIENT);
        ev->SetDestination("downstream-service");
        ev->SetEndPoint("downstream:8080");
        auto ann = ev->GetAnnotations();
        if (ann) {
            ann->AppendString(pinpoint::ANNOTATION_HTTP_URL,
                              "http://downstream:8080/api/data");
            ann->AppendInt(pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);
        }
    }
    ev->EndEvent();

    // Async span — the worker is joined so the endpoint has deterministic
    // completion and shutdown never races a detached tracing thread.
    auto prepare_ev = span->NewSpanEvent("prepare_async");
    auto async_span = span->NewAsyncSpan("async_task");

    std::thread async_worker([async_span]() {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> count_dist(1, 20);
        std::uniform_int_distribution<int> sleep_dist(1, 50);
        int count = count_dist(rng);
        for (int i = 0; i < count; ++i) {
            auto async_ev = async_span->NewSpanEvent("async_work_" + std::to_string(i));
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_dist(rng)));
            async_ev->EndEvent();
        }
        async_span->EndSpan();
    });
    prepare_ev->EndEvent();

    // More sequential events (random 1~20)
    {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> count_dist(1, 20);
        std::uniform_int_distribution<int> sleep_dist(1, 50);
        int count = count_dist(rng);
        for (int i = 0; i < count; ++i) {
            auto post_ev = span->NewSpanEvent("post_process_" + std::to_string(i));
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_dist(rng)));
            post_ev->EndEvent();
        }
    }

    async_worker.join();

    res.status = 200;
    res.set_content("mixed", "text/plain");
    finish_span(req, res, span);
}

// Error endpoint: simulates error spans
void on_error(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);

    auto ev = span->NewSpanEvent("failing_operation");
    if (ev) {
        ev->SetError("ConnectionTimeout", "simulated error: connection timeout");
    }
    ev->EndEvent();

    span->SetError("Internal Server Error");
    res.status = 500;
    res.set_content("error", "text/plain");
    finish_span(req, res, span);
}

class IntegrationCallStack final : public pinpoint::CallStackReader {
 public:
    void ForEach(std::function<void(std::string_view, std::string_view,
                                    std::string_view, int)> callback) const override {
        callback("it_test_server", "on_features", __FILE__, __LINE__);
        callback("it_test_server", "feature_failure", __FILE__, __LINE__);
    }
};

// Deterministic coverage of the public C++ tracing API. The response exposes
// locally verifiable invariants; serialized metadata is then sent to the live
// collector by the agent.
void on_features(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);
    span->SetServiceType(pinpoint::SERVICE_TYPE_CPP);
    span->SetStartTime(std::chrono::system_clock::now() -
                       std::chrono::milliseconds(2));
    span->SetAcceptorHost(req.get_header_value("Host"));

    const pinpoint::SqlUid sql_uid = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    auto span_annotations = span->GetAnnotations();
    span_annotations->AppendInt(9100, 42);
    span_annotations->AppendLong(9101, 1234567890123LL);
    span_annotations->AppendString(9102, "cpp-it-feature-span");
    span_annotations->AppendStringString(9103, "feature-key", "feature-value");
    span_annotations->AppendIntStringString(9104, 7, "left", "right");
    span_annotations->AppendSqlUidStringString(
        9105, sql_uid, "SELECT * FROM feature WHERE id = ?", "17");
    span_annotations->AppendLongIntIntByteByteString(
        9106, 123456789LL, 10, 20, 1, 2, "network-detail");

    auto feature_event = span->NewSpanEvent(
        "feature-event-initial", pinpoint::SERVICE_TYPE_CPP_FUNC);
    const bool active_event_observed = span->GetSpanEvent() == feature_event;
    feature_event->SetOperationName("feature-event");
    feature_event->SetStartTime(std::chrono::system_clock::now() -
                                std::chrono::milliseconds(1));
    feature_event->SetDestination("feature-destination");
    feature_event->SetEndPoint("feature-endpoint:1234");
    auto event_annotations = feature_event->GetAnnotations();
    event_annotations->AppendInt(9200, -1);
    event_annotations->AppendLong(9201, -9876543210LL);
    event_annotations->AppendString(9202, "event-string");
    event_annotations->AppendStringString(9203, "event-key", "event-value");
    event_annotations->AppendIntStringString(9204, 99, "event-left", "event-right");
    event_annotations->AppendSqlUidStringString(9205, sql_uid, "sql", "bind");
    event_annotations->AppendLongIntIntByteByteString(
        9206, 5, 6, 7, 8, 9, "event-network");
    feature_event->EndEvent();

    auto sql_event = span->NewSpanEvent("feature-sql", pinpoint::SERVICE_TYPE_MYSQL_QUERY);
    sql_event->SetDestination("feature-db");
    sql_event->SetEndPoint("127.0.0.1:3306");
    sql_event->SetSqlQuery(
        "SELECT name FROM users WHERE id = ? AND role = ? /* it */",
        {17, "admin"});
    sql_event->EndEvent();

    IntegrationCallStack callstack;
    auto error_event = span->NewSpanEvent("feature-callstack-error");
    error_event->SetError("FeatureFailure", "deterministic feature failure", callstack);
    error_event->EndEvent();

    httplib::Headers logging_headers;
    HttpHeaderReaderWriter logging_writer(logging_headers);
    span->SetLogging(logging_writer);
    const bool logging_context =
        logging_headers.find("PtxId") != logging_headers.end() &&
        logging_headers.find("PspanId") != logging_headers.end();

    httplib::Headers injected_headers;
    HttpHeaderReaderWriter injected_writer(injected_headers);
    auto inject_event = span->NewSpanEvent("feature-context-injection");
    inject_event->SetDestination("feature-context-target:8080");
    inject_event->InjectContext(injected_writer);
    inject_event->EndEvent();
    const bool context_injected =
        injected_headers.find(std::string(pinpoint::HEADER_TRACE_ID)) !=
            injected_headers.end() &&
        injected_headers.find(std::string(pinpoint::HEADER_SPAN_ID)) !=
            injected_headers.end() &&
        injected_headers.find(std::string(pinpoint::HEADER_PARENT_SPAN_ID)) !=
            injected_headers.end();

    auto async_span = span->NewAsyncSpan("feature-async");
    const std::string parent_trace_id = span->GetTraceId();
    bool async_complete = false;
    bool async_trace_matches = false;
    std::thread async_worker([&async_span, &async_complete,
                              &async_trace_matches, &parent_trace_id]() {
        auto event = async_span->NewSpanEvent("feature-async-work");
        event->GetAnnotations()->AppendString(pinpoint::ANNOTATION_API,
                                               "feature-async-work");
        event->EndEvent();
        async_trace_matches = async_span->GetTraceId() == parent_trace_id;
        async_span->EndSpan();
        async_complete = true;
    });
    async_worker.join();

    std::ostringstream body;
    body << "{"
         << "\"status\":\"ok\","
         << "\"sampled\":" << it_test::JsonBool(span->IsSampled()) << ","
         << "\"trace_id\":\"" << it_test::JsonEscape(span->GetTraceId()) << "\","
         << "\"span_id\":\"" << span->GetSpanId() << "\","
         << "\"active_event_observed\":"
         << it_test::JsonBool(active_event_observed) << ","
         << "\"logging_context\":" << it_test::JsonBool(logging_context) << ","
         << "\"context_injected\":" << it_test::JsonBool(context_injected) << ","
         << "\"async_complete\":" << it_test::JsonBool(async_complete) << ","
         << "\"async_trace_matches\":"
         << it_test::JsonBool(async_trace_matches) << "}";

    res.status = 200;
    res.set_header("X-Response-Time", "2ms");
    res.set_header("X-Request-ID", req.get_header_value("X-Request-ID"));
    res.set_content(body.str(), "application/json");
    finish_span(req, res, span);
}

// Real HTTP client instrumentation and cross-process context propagation.
void on_http_client(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);
    const std::string path = req.has_param("error") ? "/error" : "/trace";
    const std::string url = "http://" + g_http_target + path;

    httplib::Headers headers = {
        {"User-Agent", "pinpoint-cpp-it-test"},
        {"Content-Type", "application/json"},
        {"X-Request-ID", req.get_header_value("X-Request-ID")},
        {"Cookie", "session_id=it-session; token=it-token"},
    };
    httplib::Headers cookies = {
        {"session_id", "it-session"}, {"token", "it-token"}};

    auto event = span->NewSpanEvent("http-downstream-call");
    HttpHeaderReader request_reader(headers);
    HttpHeaderReader cookie_reader(cookies);
    pinpoint::helper::TraceHttpClientRequest(
        event, g_http_target, url, request_reader, cookie_reader);
    HttpHeaderReaderWriter context_writer(headers);
    event->InjectContext(context_writer);

    httplib::Client client(g_http_target);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    client.set_write_timeout(5, 0);
    auto result = client.Get(path, headers);

    int downstream_status = 0;
    std::string downstream_body = "{}";
    bool trace_id_matches = false;
    bool parent_span_matches = false;
    bool downstream_verified = false;
    if (result) {
        downstream_status = result->status;
        downstream_body = result->body;
        HttpHeaderReader response_reader(result->headers);
        pinpoint::helper::TraceHttpClientResponse(
            event, result->status, response_reader);
        trace_id_matches =
            result->get_header_value("X-It-Trace-Id") == span->GetTraceId();
        parent_span_matches = downstream_body.find(
            "\"incoming_parent_span_id\":\"" +
            std::to_string(span->GetSpanId()) + "\"") != std::string::npos;
        downstream_verified =
            downstream_body.find("\"trace_id_matches\":true") != std::string::npos &&
            downstream_body.find("\"span_id_matches\":true") != std::string::npos;
    } else {
        const auto error = httplib::to_string(result.error());
        event->SetError("HttpClientError", error);
        span->SetError("HttpClientError", error);
    }
    event->EndEvent();

    const bool propagated = span->IsSampled() && trace_id_matches &&
                            parent_span_matches && downstream_verified;
    std::ostringstream body;
    body << "{"
         << "\"status\":\"" << (result ? "ok" : "error") << "\","
         << "\"sampled\":" << it_test::JsonBool(span->IsSampled()) << ","
         << "\"trace_id\":\"" << it_test::JsonEscape(span->GetTraceId()) << "\","
         << "\"span_id\":\"" << span->GetSpanId() << "\","
         << "\"downstream_status\":" << downstream_status << ","
         << "\"trace_id_matches\":" << it_test::JsonBool(trace_id_matches) << ","
         << "\"parent_span_matches\":"
         << it_test::JsonBool(parent_span_matches) << ","
         << "\"propagated\":" << it_test::JsonBool(propagated) << ","
         << "\"downstream\":" << downstream_body << "}";
    res.status = result ? 200 : 502;
    res.set_content(body.str(), "application/json");
    finish_span(req, res, span);
}

// =============================================================================
// SQL-traced endpoints (no actual DB connection — span events only)
// =============================================================================

static void trace_sql(pinpoint::SpanPtr span, const std::string& operation,
                      const std::string& sql, const std::string& params) {
    auto ev = span->NewSpanEvent("SQL_" + operation);
    if (ev) {
        ev->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
        ev->SetEndPoint("localhost:33060");
        ev->SetDestination("test");
        if (params.empty()) {
            ev->SetSqlQuery(sql, {});
        } else {
            ev->SetSqlQuery(sql, {params});
        }
    }

    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> sleep_dist(1, 100);
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_dist(rng)));

    static const std::string error_messages[] = {
        "Connection timed out after 30s",
        "Deadlock found when trying to get lock; try restarting transaction",
        "Lost connection to MySQL server during query",
        "Table 'test." + operation + "_tmp' doesn't exist",
        "Query execution was interrupted",
        "Too many connections",
        "Lock wait timeout exceeded; try restarting transaction",
        "Duplicate entry for key 'PRIMARY'",
    };
    std::uniform_int_distribution<int> error_dist(0, 9);
    if (ev && error_dist(rng) < 3) {  // ~30% chance of error
        std::uniform_int_distribution<int> msg_dist(0, 7);
        ev->SetError("MySQL_Error", error_messages[msg_dist(rng)]);
    }

    ev->EndEvent();
}

// /db-crud: Full CRUD cycle with SQL tracing (no actual DB)
void on_db_crud(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);

    trace_sql(span, "CREATE", "CREATE TABLE IF NOT EXISTS it_test_users "
              "(id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(100), "
              "email VARCHAR(100), age INT, ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP)", "");

    trace_sql(span, "DELETE", "DELETE FROM it_test_users", "");

    std::vector<std::tuple<std::string, std::string, int>> users = {
        {"Alice", "alice@test.com", 28},
        {"Bob", "bob@test.com", 35},
        {"Charlie", "charlie@test.com", 42},
        {"Diana", "diana@test.com", 31},
        {"Eve", "eve@test.com", 24},
    };
    for (auto& u : users) {
        std::string params = std::get<0>(u) + ", " + std::get<1>(u) + ", " + std::to_string(std::get<2>(u));
        trace_sql(span, "INSERT", "INSERT INTO it_test_users (name, email, age) VALUES (?, ?, ?)", params);
    }

    trace_sql(span, "SELECT", "SELECT * FROM it_test_users ORDER BY id", "");
    trace_sql(span, "SELECT", "SELECT * FROM it_test_users WHERE age > ?", "30");
    trace_sql(span, "SELECT", "SELECT COUNT(*) as cnt, AVG(age) as avg_age FROM it_test_users", "");
    trace_sql(span, "UPDATE", "UPDATE it_test_users SET age = age + 1 WHERE name = ?", "Alice");
    trace_sql(span, "SELECT", "SELECT name, email FROM it_test_users WHERE name LIKE ?", "%a%");
    trace_sql(span, "DELETE", "DELETE FROM it_test_users WHERE age > ?", "40");
    trace_sql(span, "SELECT", "SELECT * FROM non_existent_table_xyz", "");

    res.status = 200;
    res.set_content("{\"status\":\"ok\"}", "application/json");
    finish_span(req, res, span);
}

// /db-batch: Batch insert + select SQL tracing
void on_db_batch(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);

    int batch_size = 20;
    auto bs_param = req.get_param_value("size");
    if (!bs_param.empty()) {
        batch_size = std::min(std::max(std::stoi(bs_param), 1), 200);
    }

    trace_sql(span, "CREATE", "CREATE TABLE IF NOT EXISTS it_test_batch "
              "(id INT AUTO_INCREMENT PRIMARY KEY, val VARCHAR(100), num INT)", "");

    for (int i = 0; i < batch_size; ++i) {
        std::string val = "item_" + std::to_string(i);
        trace_sql(span, "INSERT", "INSERT INTO it_test_batch (val, num) VALUES (?, ?)",
                  val + ", " + std::to_string(i));
    }

    trace_sql(span, "SELECT", "SELECT * FROM it_test_batch ORDER BY id DESC LIMIT ?",
              std::to_string(batch_size));
    trace_sql(span, "DELETE", "DELETE FROM it_test_batch", "");

    std::ostringstream oss;
    oss << "{\"batch_size\":" << batch_size << ",\"status\":\"ok\"}";
    res.status = 200;
    res.set_content(oss.str(), "application/json");
    finish_span(req, res, span);
}

// /db-complex: Complex queries with JOIN, subquery, aggregation (SQL tracing only)
void on_db_complex(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);

    trace_sql(span, "CREATE",
              "CREATE TABLE IF NOT EXISTS it_test_orders "
              "(id INT AUTO_INCREMENT PRIMARY KEY, user_id INT, amount DECIMAL(10,2), "
              "status VARCHAR(20), created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)", "");

    trace_sql(span, "CREATE",
              "CREATE TABLE IF NOT EXISTS it_test_users "
              "(id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(100), "
              "email VARCHAR(100), age INT, ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP)", "");

    trace_sql(span, "DELETE", "DELETE FROM it_test_orders", "");
    trace_sql(span, "DELETE", "DELETE FROM it_test_users", "");

    const char* insert_user = "INSERT INTO it_test_users (name, email, age) VALUES (?, ?, ?)";
    trace_sql(span, "INSERT", insert_user, "Alice, alice@t.com, 28");
    trace_sql(span, "INSERT", insert_user, "Bob, bob@t.com, 35");
    trace_sql(span, "INSERT", insert_user, "Charlie, charlie@t.com, 42");

    const char* insert_order = "INSERT INTO it_test_orders (user_id, amount, status) VALUES (?, ?, ?)";
    trace_sql(span, "INSERT", insert_order, "1, 99.50, completed");
    trace_sql(span, "INSERT", insert_order, "1, 150.00, pending");
    trace_sql(span, "INSERT", insert_order, "2, 200.00, completed");
    trace_sql(span, "INSERT", insert_order, "3, 75.25, completed");

    trace_sql(span, "SELECT",
              "SELECT u.name, o.amount, o.status FROM it_test_users u "
              "JOIN it_test_orders o ON u.id = o.user_id ORDER BY o.amount DESC", "");

    trace_sql(span, "SELECT",
              "SELECT u.name, COUNT(o.id) as order_count, SUM(o.amount) as total "
              "FROM it_test_users u LEFT JOIN it_test_orders o ON u.id = o.user_id "
              "GROUP BY u.id, u.name ORDER BY total DESC", "");

    trace_sql(span, "SELECT",
              "SELECT name, email FROM it_test_users WHERE id IN "
              "(SELECT DISTINCT user_id FROM it_test_orders WHERE status = ?)", "completed");

    trace_sql(span, "SELECT",
              "SELECT name, age, CASE WHEN age < 30 THEN 'Young' "
              "WHEN age < 40 THEN 'Middle' ELSE 'Senior' END as age_group "
              "FROM it_test_users ORDER BY age", "");

    res.status = 200;
    res.set_content("{\"status\":\"ok\",\"queries\":\"complex\"}", "application/json");
    finish_span(req, res, span);
}


// =============================================================================
// gRPC client endpoints
// =============================================================================
static std::string g_grpc_target = "localhost:50051";
static std::unique_ptr<grpcdemo::Hello::Stub> g_grpc_stub;

struct GrpcOutcome {
    bool ok = false;
    bool propagated = false;
    int count = 0;
    std::string trace_id;
    std::string error;
};

static void set_grpc_deadline(grpc::ClientContext& context) {
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(5));
}

static bool grpc_context_matches(const pinpoint::SpanPtr& parent,
                                 const grpcdemo::Greeting& response) {
    return parent->IsSampled() && response.sampled() &&
           !response.trace_id().empty() &&
           response.trace_id() == parent->GetTraceId() &&
           response.span_id() != 0;
}

static void init_grpc_stub() {
    grpc::ChannelArguments args;
    std::vector<std::unique_ptr<grpc::experimental::ClientInterceptorFactoryInterface>>
        interceptor_creators;
    interceptor_creators.push_back(
        std::make_unique<grpc_demo::PinpointClientInterceptorFactory>());

    auto channel = grpc::experimental::CreateCustomChannelWithInterceptors(
        g_grpc_target, grpc::InsecureChannelCredentials(), args,
        std::move(interceptor_creators));
    g_grpc_stub = grpcdemo::Hello::NewStub(channel);
}

static GrpcOutcome call_grpc_unary(const pinpoint::SpanPtr& span,
                                   const std::string& message) {
    grpc::ClientContext context;
    set_grpc_deadline(context);
    grpcdemo::Greeting request;
    grpcdemo::Greeting response;
    request.set_msg(message);
    auto status = g_grpc_stub->UnaryCallUnaryReturn(&context, request, &response);
    return {status.ok(), status.ok() && grpc_context_matches(span, response),
            status.ok() ? 1 : 0, response.trace_id(), status.error_message()};
}

static GrpcOutcome call_grpc_server_stream(const pinpoint::SpanPtr& span) {
    grpc::ClientContext context;
    set_grpc_deadline(context);
    grpcdemo::Greeting request;
    request.set_msg("Stream greetings from it-test");
    auto reader = g_grpc_stub->UnaryCallStreamReturn(&context, request);
    grpcdemo::Greeting response;
    int count = 0;
    bool propagated = true;
    std::string trace_id;
    while (reader->Read(&response)) {
        propagated = propagated && grpc_context_matches(span, response);
        trace_id = response.trace_id();
        ++count;
    }
    auto status = reader->Finish();
    return {status.ok(), status.ok() && count == 3 && propagated,
            count, trace_id, status.error_message()};
}

static GrpcOutcome call_grpc_client_stream(const pinpoint::SpanPtr& span,
                                           int count) {
    grpc::ClientContext context;
    set_grpc_deadline(context);
    grpcdemo::Greeting response;
    auto writer = g_grpc_stub->StreamCallUnaryReturn(&context, &response);
    int written = 0;
    for (int i = 0; i < count; ++i) {
        grpcdemo::Greeting request;
        request.set_msg("Client stream " + std::to_string(i));
        if (writer->Write(request)) ++written;
    }
    writer->WritesDone();
    const auto status = writer->Finish();
    return {status.ok(), status.ok() && written == count &&
                            grpc_context_matches(span, response),
            written, response.trace_id(), status.error_message()};
}

static GrpcOutcome call_grpc_bidi(const pinpoint::SpanPtr& span, int count) {
    grpc::ClientContext context;
    set_grpc_deadline(context);
    auto stream = g_grpc_stub->StreamCallStreamReturn(&context);
    int received = 0;
    bool propagated = true;
    std::string trace_id;
    for (int i = 0; i < count; ++i) {
        grpcdemo::Greeting request;
        request.set_msg("Message " + std::to_string(i));
        if (!stream->Write(request)) break;
        grpcdemo::Greeting response;
        if (stream->Read(&response)) {
            propagated = propagated && grpc_context_matches(span, response);
            trace_id = response.trace_id();
            ++received;
        }
    }
    stream->WritesDone();
    auto status = stream->Finish();
    return {status.ok(), status.ok() && received == count && propagated,
            received, trace_id, status.error_message()};
}

static void write_grpc_response(const httplib::Request& req,
                                httplib::Response& res,
                                const pinpoint::SpanPtr& span,
                                const std::string& method,
                                const GrpcOutcome& outcome,
                                bool expected_error = false) {
    std::ostringstream body;
    body << "{\"method\":\"" << method << "\","
         << "\"ok\":" << it_test::JsonBool(outcome.ok) << ","
         << "\"propagated\":" << it_test::JsonBool(outcome.propagated) << ","
         << "\"count\":" << outcome.count << ","
         << "\"trace_id\":\"" << it_test::JsonEscape(outcome.trace_id) << "\","
         << "\"error\":\"" << it_test::JsonEscape(outcome.error) << "\","
         << "\"expected_error\":" << it_test::JsonBool(expected_error) << "}";
    res.status = (outcome.ok || expected_error) ? 200 : 502;
    res.set_content(body.str(), "application/json");
    grpc_demo::ClearCurrentSpan();
    finish_span(req, res, span);
}

void on_grpc_unary(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);
    grpc_demo::SetCurrentSpan(span);
    write_grpc_response(req, res, span, "unary",
                        call_grpc_unary(span, "Hello from it-test unary"));
}

void on_grpc_stream(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);
    grpc_demo::SetCurrentSpan(span);
    write_grpc_response(req, res, span, "server_stream",
                        call_grpc_server_stream(span));
}

void on_grpc_client_stream(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);
    grpc_demo::SetCurrentSpan(span);
    int count = 3;
    if (req.has_param("count")) {
        count = std::min(std::max(std::stoi(req.get_param_value("count")), 1), 20);
    }
    write_grpc_response(req, res, span, "client_stream",
                        call_grpc_client_stream(span, count));
}

void on_grpc_bidi(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);
    grpc_demo::SetCurrentSpan(span);
    int count = 3;
    if (req.has_param("count")) {
        count = std::min(std::max(std::stoi(req.get_param_value("count")), 1), 20);
    }
    write_grpc_response(req, res, span, "bidi", call_grpc_bidi(span, count));
}

void on_grpc_error(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);
    grpc_demo::SetCurrentSpan(span);
    const auto outcome = call_grpc_unary(span, "force-error");
    write_grpc_response(req, res, span, "error", outcome,
                        !outcome.ok && !outcome.error.empty());
}

void on_grpc_all(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);
    grpc_demo::SetCurrentSpan(span);
    const auto unary = call_grpc_unary(span, "all-test unary");
    const auto server_stream = call_grpc_server_stream(span);
    const auto client_stream = call_grpc_client_stream(span, 3);
    const auto bidi = call_grpc_bidi(span, 3);
    const bool ok = unary.ok && server_stream.ok && client_stream.ok && bidi.ok;
    const bool propagated = unary.propagated && server_stream.propagated &&
                            client_stream.propagated && bidi.propagated;
    std::ostringstream body;
    body << "{\"method\":\"all\",\"ok\":" << it_test::JsonBool(ok)
         << ",\"propagated\":" << it_test::JsonBool(propagated)
         << ",\"methods\":4,\"server_stream_count\":" << server_stream.count
         << ",\"client_stream_count\":" << client_stream.count
         << ",\"bidi_count\":" << bidi.count << "}";
    res.status = ok ? 200 : 502;
    res.set_content(body.str(), "application/json");
    grpc_demo::ClearCurrentSpan();
    finish_span(req, res, span);
}

// =============================================================================
// Agent lifecycle endpoints
// =============================================================================
void on_agent_start(const httplib::Request&, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(g_agent_mutex);
    if (g_agent) {
        res.status = 200;
        res.set_content("{\"status\":\"already_running\"}", "application/json");
        return;
    }
    if (!pinpoint::StartAgent()) {
        std::cerr << "pinpoint agent start failed; check the agent log" << std::endl;
        res.status = 500;
        res.set_content("{\"status\":\"start_failed\"}", "application/json");
        return;
    }
    g_agent = pinpoint::GlobalAgent();
    res.status = 200;
    res.set_content("{\"status\":\"started\"}", "application/json");
}

void on_agent_shutdown(const httplib::Request&, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(g_agent_mutex);
    if (!g_agent) {
        res.status = 200;
        res.set_content("{\"status\":\"not_running\"}", "application/json");
        return;
    }
    g_agent->Shutdown();
    g_agent.reset();
    res.status = 200;
    res.set_content("{\"status\":\"shutdown\"}", "application/json");
}

void on_agent_reload(const httplib::Request& req, httplib::Response& res) {
    int counter_rate = 1;
    if (req.has_param("counter_rate")) {
        counter_rate = std::min(
            std::max(std::stoi(req.get_param_value("counter_rate")), 0), 1000);
    }
    std::ostringstream config;
    config << "Sampling:\n"
           << "  Type: COUNTER\n"
           << "  CounterRate: " << counter_rate << "\n"
           << "Log:\n"
           << "  Level: debug\n";
    std::lock_guard<std::mutex> lock(g_agent_mutex);
    // Runtime reconfiguration flows through the config-file watcher in
    // production; this test server simulates it by restarting the agent with
    // the new configuration (a fresh agent instance).
    if (g_agent) {
        g_agent->Shutdown();
        g_agent.reset();
    }
    pinpoint::AgentOptions options;
    options.config_yaml = config.str();
    if (!pinpoint::StartAgent(options)) {
        std::cerr << "pinpoint agent start failed; check the agent log" << std::endl;
    }
    g_agent = pinpoint::GlobalAgent();
    std::ostringstream body;
    body << "{\"status\":\"reloaded\",\"counter_rate\":" << counter_rate
         << ",\"agent_enabled\":" << it_test::JsonBool(g_agent->Enable()) << "}";
    res.set_content(body.str(), "application/json");
}

void on_sampling_probe(const httplib::Request& req, httplib::Response& res) {
    int count = 20;
    if (req.has_param("count")) {
        count = std::min(std::max(std::stoi(req.get_param_value("count")), 1), 1000);
    }
    int sampled = 0;
    for (int i = 0; i < count; ++i) {
        auto span = pinpoint::GlobalAgent()->NewSpan(
            "sampling-probe", "/sampling-probe/" + std::to_string(i));
        if (span->IsSampled()) ++sampled;
        span->EndSpan();
    }
    std::ostringstream body;
    body << "{\"status\":\"ok\",\"total\":" << count
         << ",\"sampled\":" << sampled
         << ",\"unsampled\":" << (count - sampled) << "}";
    res.set_content(body.str(), "application/json");
}

// =============================================================================
// Stats endpoint (no tracing)
// =============================================================================
void on_stats(const httplib::Request&, httplib::Response& res) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
    uint64_t total = total_requests.load();

    std::ostringstream oss;
    oss << "{"
        << "\"uptime_seconds\":" << elapsed << ","
        << "\"total_requests\":" << total << ","
        << "\"active_requests\":" << active_requests.load() << ","
        << "\"agent_enabled\":"
        << it_test::JsonBool(pinpoint::GlobalAgent()->Enable()) << ","
        << "\"collector_host\":\""
        << it_test::JsonEscape(it_test::CollectorHost()) << "\","
        << "\"requests_per_second\":"
        << (elapsed > 0 ? static_cast<double>(total) / elapsed : 0.0)
        << "}";

    res.set_content(oss.str(), "application/json");
}

void on_ready(const httplib::Request&, httplib::Response& res) {
    const bool enabled = pinpoint::GlobalAgent()->Enable();
    res.status = enabled ? 200 : 503;
    res.set_content(std::string("{\"status\":\"") +
                        (enabled ? "ready" : "waiting_for_collector") +
                        "\",\"agent_enabled\":" + it_test::JsonBool(enabled) +
                        ",\"collector_host\":\"" +
                        it_test::JsonEscape(it_test::CollectorHost()) + "\"}",
                    "application/json");
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char* argv[]) {
    int port = 8090;
    bool auto_start = true;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-auto-start") == 0) {
            auto_start = false;
        } else {
            port = std::atoi(argv[i]);
        }
    }

    it_test::ConfigureAgentEnvironment("cpp-it-http-upstream",
                                       "cpp-it-http-up");

    if (auto_start) {
        pinpoint::AgentOptions options;
        options.app_type = pinpoint::APP_TYPE_CPP;
        options.server_info = "cpp-it-http-upstream";
        options.args = {"--port=" + std::to_string(port)};
        options.libs = {"cpp-httplib", "grpc"};
        if (!pinpoint::StartAgent(options)) {
            std::cerr << "pinpoint agent start failed; check the agent log" << std::endl;
        }
        g_agent = pinpoint::GlobalAgent();
    }

    httplib::Server server;

    // Agent lifecycle endpoints
    server.Post("/agent/start", on_agent_start);
    server.Post("/agent/shutdown", on_agent_shutdown);
    server.Post("/agent/reload", on_agent_reload);
    server.Get("/agent/status", on_stats);

    // HTTP-only endpoints
    server.Get("/simple", on_simple);
    server.Get("/deep", on_deep);
    server.Get("/wide", on_wide);
    server.Get("/annotated", on_annotated);
    server.Get("/features", on_features);
    server.Get("/mixed", on_mixed);
    server.Get("/error", on_error);
    server.Get("/http-client", on_http_client);
    server.Get("/sampling-probe", on_sampling_probe);
    server.Get("/stats", on_stats);
    server.Get("/ready", on_ready);

    // gRPC client endpoints
    if (getenv("GRPC_TARGET")) g_grpc_target = getenv("GRPC_TARGET");
    if (getenv("HTTP_TARGET")) g_http_target = getenv("HTTP_TARGET");
    init_grpc_stub();
    server.Get("/grpc-unary", on_grpc_unary);
    server.Get("/grpc-stream", on_grpc_stream);
    server.Get("/grpc-client-stream", on_grpc_client_stream);
    server.Get("/grpc-bidi", on_grpc_bidi);
    server.Get("/grpc-error", on_grpc_error);
    server.Get("/grpc-all", on_grpc_all);
    printf("gRPC client endpoints enabled (target=%s)\n", g_grpc_target.c_str());
    printf("HTTP client endpoints enabled (target=%s)\n", g_http_target.c_str());

    // SQL-traced endpoints (no actual DB connection)
    server.Get("/db-crud", on_db_crud);
    server.Get("/db-batch", on_db_batch);
    server.Get("/db-complex", on_db_complex);

    server.Post("/server/shutdown", [&](const httplib::Request&,
                                         httplib::Response& res) {
        res.set_content("{\"status\":\"shutting_down\"}", "application/json");
        std::thread([&server]() { server.stop(); }).detach();
    });

    printf("\nIntegration test server starting on port %d\n", port);
    printf("Endpoints:\n");
    printf("  GET /simple          - minimal span\n");
    printf("  GET /deep?depth=N    - deeply nested span events (default 20, max 256)\n");
    printf("  GET /wide?width=N    - many sequential span events (default 50, max 10000)\n");
    printf("  GET /annotated       - annotation-heavy spans\n");
    printf("  GET /features        - deterministic full C++ API feature coverage\n");
    printf("  GET /http-client     - real HTTP downstream trace propagation\n");
    printf("  GET /mixed           - combined workload (SQL trace + HTTP client + async)\n");
    printf("  GET /error           - error spans\n");
    printf("  GET /stats           - server metrics (no tracing)\n");
    printf("  GET /grpc-unary      - gRPC unary call to grpc_server\n");
    printf("  GET /grpc-stream     - gRPC server-streaming call\n");
    printf("  GET /grpc-client-stream?count=N - gRPC client-streaming call\n");
    printf("  GET /grpc-bidi?count=N - gRPC bidirectional streaming (default 3, max 20)\n");
    printf("  GET /grpc-error      - expected gRPC error propagation\n");
    printf("  GET /grpc-all        - all four gRPC methods in sequence\n");
    printf("  GET /db-crud         - SQL trace CRUD cycle (no actual DB)\n");
    printf("  GET /db-batch?size=N - SQL trace batch insert+select (default 20, max 200)\n");
    printf("  GET /db-complex      - SQL trace JOIN, subquery, aggregation\n");

    printf("  POST /agent/start       - start the Pinpoint agent\n");
    printf("  POST /agent/shutdown    - shutdown the Pinpoint agent\n");
    printf("  POST /agent/reload?counter_rate=N - reload sampling config\n");
    if (!auto_start) {
        printf("\nAgent auto-start disabled. Call POST /agent/start to begin tracing.\n");
    }

    printf("Collector: %s\n", it_test::CollectorHost().c_str());
    if (!server.listen("0.0.0.0", port)) {
        fprintf(stderr, "Failed to start integration test server on port %d\n", port);
        return 1;
    }

    std::lock_guard<std::mutex> lock(g_agent_mutex);
    if (g_agent) {
        g_agent->Shutdown();
        g_agent.reset();
    }
}
