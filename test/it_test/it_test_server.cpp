#include <atomic>
#include <chrono>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <memory>
#include <functional>
#include <cstring>

#include <grpcpp/grpcpp.h>

#include "pinpoint/tracer.h"
#include "httplib.h"
#include "http_trace_context.h"

#include "pinpoint_grpc_context.h"
#include "pinpoint_grpc_interceptors.h"
#include "testapp.grpc.pb.h"

// =============================================================================
// Global state
// =============================================================================
static std::atomic<uint64_t> total_requests{0};
static std::atomic<uint64_t> active_requests{0};
static auto start_time = std::chrono::steady_clock::now();

// =============================================================================
// Common helpers
// =============================================================================
pinpoint::SpanPtr make_span(const httplib::Request& req) {
    auto agent = pinpoint::GlobalAgent();
    HttpHeaderReader h_reader(req.headers);
    auto span = agent->NewSpan("it-test-server", req.path, req.method, h_reader);

    auto end_point = req.get_header_value("Host");
    if (end_point.empty()) {
        end_point = req.local_addr + ":" + std::to_string(req.local_port);
    }
    pinpoint::helper::TraceHttpServerRequest(span, req.remote_addr, end_point, h_reader);
    return span;
}

void finish_span(const httplib::Request& req, httplib::Response& res,
                 pinpoint::SpanPtr span) {
    HttpHeaderReader http_reader(res.headers);
    span->RecordHeader(pinpoint::HTTP_RESPONSE, http_reader);
    span->SetStatusCode(res.status);
    span->SetUrlStat(req.path, req.method, res.status);
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

    span->NewSpanEvent("simple_work");
    span->EndSpanEvent();

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
        depth = std::min(std::max(std::stoi(depth_param), 1), 60);
    }

    for (int i = 0; i < depth; ++i) {
        span->NewSpanEvent("deep_level_" + std::to_string(i));
    }
    for (int i = 0; i < depth; ++i) {
        span->EndSpanEvent();
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
        width = std::min(std::max(std::stoi(width_param), 1), 500);
    }

    for (int i = 0; i < width; ++i) {
        span->NewSpanEvent("wide_event_" + std::to_string(i));
        span->EndSpanEvent();
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
        span->NewSpanEvent("annotated_op_" + std::to_string(i));
        auto ev = span->GetSpanEvent();
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
        span->EndSpanEvent();
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
    span->NewSpanEvent("db_query");
    auto ev = span->GetSpanEvent();
    if (ev) {
        ev->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
        ev->SetEndPoint("localhost:3306");
        ev->SetDestination("test");
        ev->SetSqlQuery("SELECT * FROM users WHERE id = ?", "42");
    }
    span->NewSpanEvent("db_parse");
    span->EndSpanEvent();
    span->EndSpanEvent();

    // Simulate HTTP client call
    span->NewSpanEvent("http_client_call");
    ev = span->GetSpanEvent();
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
    span->EndSpanEvent();

    // Async span — fire-and-forget background work in a separate thread
    span->NewSpanEvent("prepare_async");
    auto async_span = span->NewAsyncSpan("async_task");

    std::thread([async_span]() {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> count_dist(1, 20);
        std::uniform_int_distribution<int> sleep_dist(1, 50);
        int count = count_dist(rng);
        for (int i = 0; i < count; ++i) {
            async_span->NewSpanEvent("async_work_" + std::to_string(i));
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_dist(rng)));
            async_span->EndSpanEvent();
        }
        async_span->EndSpan();
    }).detach();
    span->EndSpanEvent();

    // More sequential events (random 1~20)
    {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> count_dist(1, 20);
        std::uniform_int_distribution<int> sleep_dist(1, 50);
        int count = count_dist(rng);
        for (int i = 0; i < count; ++i) {
            span->NewSpanEvent("post_process_" + std::to_string(i));
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_dist(rng)));
            span->EndSpanEvent();
        }
    }

    res.status = 200;
    res.set_content("mixed", "text/plain");
    finish_span(req, res, span);
}

// Error endpoint: simulates error spans
void on_error(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);

    span->NewSpanEvent("failing_operation");
    auto ev = span->GetSpanEvent();
    if (ev) {
        ev->SetError("ConnectionTimeout", "simulated error: connection timeout");
    }
    span->EndSpanEvent();

    span->SetError("Internal Server Error");
    res.status = 500;
    res.set_content("error", "text/plain");
    finish_span(req, res, span);
}

// =============================================================================
// SQL-traced endpoints (no actual DB connection — span events only)
// =============================================================================

static void trace_sql(pinpoint::SpanPtr span, const std::string& operation,
                      const std::string& sql, const std::string& params) {
    span->NewSpanEvent("SQL_" + operation);
    auto ev = span->GetSpanEvent();
    if (ev) {
        ev->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
        ev->SetEndPoint("localhost:33060");
        ev->SetDestination("test");
        ev->SetSqlQuery(sql, params);
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

    span->EndSpanEvent();
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

// /grpc-unary: Call gRPC Unary method
void on_grpc_unary(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);
    grpc_demo::SetCurrentSpan(span);

    span->NewSpanEvent("grpc_unary_call");

    grpc::ClientContext context;
    grpcdemo::Greeting request;
    grpcdemo::Greeting response;
    request.set_msg("Hello from it-test unary");

    auto status = g_grpc_stub->UnaryCallUnaryReturn(&context, request, &response);

    auto ev = span->GetSpanEvent();
    if (ev) {
        if (!status.ok()) {
            ev->SetError("gRPC_Error", status.error_message());
        } else {
            ev->GetAnnotations()->AppendString(pinpoint::ANNOTATION_API, response.msg());
        }
    }
    span->EndSpanEvent();

    if (status.ok()) {
        res.status = 200;
        res.set_content("{\"method\":\"unary\",\"response\":\"" + response.msg() + "\"}",
                        "application/json");
    } else {
        res.status = 502;
        res.set_content("{\"method\":\"unary\",\"error\":\"" + status.error_message() + "\"}",
                        "application/json");
    }

    grpc_demo::ClearCurrentSpan();
    finish_span(req, res, span);
}

// /grpc-stream: Call gRPC Server-Streaming method
void on_grpc_stream(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);
    grpc_demo::SetCurrentSpan(span);

    span->NewSpanEvent("grpc_server_stream_call");

    grpc::ClientContext context;
    grpcdemo::Greeting request;
    request.set_msg("Stream greetings from it-test");

    auto reader = g_grpc_stub->UnaryCallStreamReturn(&context, request);
    grpcdemo::Greeting response;
    int count = 0;
    std::ostringstream msgs;
    msgs << "[";
    while (reader->Read(&response)) {
        if (count > 0) msgs << ",";
        msgs << "\"" << response.msg() << "\"";
        ++count;
    }
    msgs << "]";

    auto status = reader->Finish();
    auto ev = span->GetSpanEvent();
    if (ev) {
        if (!status.ok()) {
            ev->SetError("gRPC_Error", status.error_message());
        }
    }
    span->EndSpanEvent();

    if (status.ok()) {
        res.status = 200;
        res.set_content("{\"method\":\"server_stream\",\"count\":" + std::to_string(count) +
                        ",\"messages\":" + msgs.str() + "}", "application/json");
    } else {
        res.status = 502;
        res.set_content("{\"method\":\"server_stream\",\"error\":\"" +
                        status.error_message() + "\"}", "application/json");
    }

    grpc_demo::ClearCurrentSpan();
    finish_span(req, res, span);
}

// /grpc-bidi: Call gRPC Bidirectional-Streaming method
void on_grpc_bidi(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);
    grpc_demo::SetCurrentSpan(span);

    span->NewSpanEvent("grpc_bidi_stream_call");

    grpc::ClientContext context;
    auto stream = g_grpc_stub->StreamCallStreamReturn(&context);

    int count = 3;
    auto count_param = req.get_param_value("count");
    if (!count_param.empty()) {
        count = std::min(std::max(std::stoi(count_param), 1), 20);
    }

    std::ostringstream msgs;
    msgs << "[";
    for (int i = 0; i < count; ++i) {
        grpcdemo::Greeting request;
        request.set_msg("Message " + std::to_string(i));
        stream->Write(request);
        grpcdemo::Greeting response;
        if (stream->Read(&response)) {
            if (i > 0) msgs << ",";
            msgs << "\"" << response.msg() << "\"";
        }
    }
    msgs << "]";
    stream->WritesDone();

    auto status = stream->Finish();
    auto ev = span->GetSpanEvent();
    if (ev) {
        if (!status.ok()) {
            ev->SetError("gRPC_Error", status.error_message());
        }
    }
    span->EndSpanEvent();

    if (status.ok()) {
        res.status = 200;
        res.set_content("{\"method\":\"bidi\",\"count\":" + std::to_string(count) +
                        ",\"messages\":" + msgs.str() + "}", "application/json");
    } else {
        res.status = 502;
        res.set_content("{\"method\":\"bidi\",\"error\":\"" +
                        status.error_message() + "\"}", "application/json");
    }

    grpc_demo::ClearCurrentSpan();
    finish_span(req, res, span);
}

// /grpc-all: Call all three gRPC methods in sequence
void on_grpc_all(const httplib::Request& req, httplib::Response& res) {
    RequestTracker rt;
    auto span = make_span(req);
    grpc_demo::SetCurrentSpan(span);

    std::ostringstream result;
    result << "{\"results\":[";

    // 1. Unary
    {
        span->NewSpanEvent("grpc_unary");
        grpc::ClientContext ctx;
        grpcdemo::Greeting rq, rs;
        rq.set_msg("all-test unary");
        auto st = g_grpc_stub->UnaryCallUnaryReturn(&ctx, rq, &rs);
        auto ev = span->GetSpanEvent();
        if (ev && !st.ok()) ev->SetError("gRPC_Error", st.error_message());
        span->EndSpanEvent();
        result << "{\"method\":\"unary\",\"ok\":" << (st.ok() ? "true" : "false") << "}";
    }

    result << ",";

    // 2. Server streaming
    {
        span->NewSpanEvent("grpc_server_stream");
        grpc::ClientContext ctx;
        grpcdemo::Greeting rq;
        rq.set_msg("all-test stream");
        auto reader = g_grpc_stub->UnaryCallStreamReturn(&ctx, rq);
        grpcdemo::Greeting rs;
        int n = 0;
        while (reader->Read(&rs)) ++n;
        auto st = reader->Finish();
        auto ev = span->GetSpanEvent();
        if (ev && !st.ok()) ev->SetError("gRPC_Error", st.error_message());
        span->EndSpanEvent();
        result << "{\"method\":\"server_stream\",\"ok\":" << (st.ok() ? "true" : "false")
               << ",\"count\":" << n << "}";
    }

    result << ",";

    // 3. Bidirectional streaming
    {
        span->NewSpanEvent("grpc_bidi_stream");
        grpc::ClientContext ctx;
        auto stream = g_grpc_stub->StreamCallStreamReturn(&ctx);
        for (int i = 0; i < 3; ++i) {
            grpcdemo::Greeting rq;
            rq.set_msg("Bidi " + std::to_string(i));
            stream->Write(rq);
            grpcdemo::Greeting rs;
            stream->Read(&rs);
        }
        stream->WritesDone();
        auto st = stream->Finish();
        auto ev = span->GetSpanEvent();
        if (ev && !st.ok()) ev->SetError("gRPC_Error", st.error_message());
        span->EndSpanEvent();
        result << "{\"method\":\"bidi\",\"ok\":" << (st.ok() ? "true" : "false") << "}";
    }

    result << "]}";
    res.status = 200;
    res.set_content(result.str(), "application/json");

    grpc_demo::ClearCurrentSpan();
    finish_span(req, res, span);
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
        << "\"requests_per_second\":"
        << (elapsed > 0 ? static_cast<double>(total) / elapsed : 0.0)
        << "}";

    res.set_content(oss.str(), "application/json");
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char* argv[]) {
    int port = 8090;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    // Config file path: use env or default
    setenv("PINPOINT_CPP_CONFIG_FILE", "/tmp/pinpoint-config.yaml", 0);
    setenv("PINPOINT_CPP_APPLICATION_NAME", "it-test-server", 0);
    setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true", 0);
    setenv("PINPOINT_CPP_SQL_ENABLE_SQL_STATS", "true", 0);

    auto agent = pinpoint::CreateAgent();

    httplib::Server server;

    // HTTP-only endpoints
    server.Get("/simple", on_simple);
    server.Get("/deep", on_deep);
    server.Get("/wide", on_wide);
    server.Get("/annotated", on_annotated);
    server.Get("/mixed", on_mixed);
    server.Get("/error", on_error);
    server.Get("/stats", on_stats);

    // gRPC client endpoints
    if (getenv("GRPC_TARGET")) g_grpc_target = getenv("GRPC_TARGET");
    init_grpc_stub();
    server.Get("/grpc-unary", on_grpc_unary);
    server.Get("/grpc-stream", on_grpc_stream);
    server.Get("/grpc-bidi", on_grpc_bidi);
    server.Get("/grpc-all", on_grpc_all);
    printf("gRPC client endpoints enabled (target=%s)\n", g_grpc_target.c_str());

    // SQL-traced endpoints (no actual DB connection)
    server.Get("/db-crud", on_db_crud);
    server.Get("/db-batch", on_db_batch);
    server.Get("/db-complex", on_db_complex);

    printf("\nIntegration test server starting on port %d\n", port);
    printf("Endpoints:\n");
    printf("  GET /simple          - minimal span\n");
    printf("  GET /deep?depth=N    - deeply nested span events (default 20, max 60)\n");
    printf("  GET /wide?width=N    - many sequential span events (default 50, max 500)\n");
    printf("  GET /annotated       - annotation-heavy spans\n");
    printf("  GET /mixed           - combined workload (SQL trace + HTTP client + async)\n");
    printf("  GET /error           - error spans\n");
    printf("  GET /stats           - server metrics (no tracing)\n");
    printf("  GET /grpc-unary      - gRPC unary call to grpc_server\n");
    printf("  GET /grpc-stream     - gRPC server-streaming call\n");
    printf("  GET /grpc-bidi?count=N - gRPC bidirectional streaming (default 3, max 20)\n");
    printf("  GET /grpc-all        - all gRPC methods in sequence\n");
    printf("  GET /db-crud         - SQL trace CRUD cycle (no actual DB)\n");
    printf("  GET /db-batch?size=N - SQL trace batch insert+select (default 20, max 200)\n");
    printf("  GET /db-complex      - SQL trace JOIN, subquery, aggregation\n");

    server.listen("0.0.0.0", port);
    agent->Shutdown();
}
