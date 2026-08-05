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

// HTTP server for the cross-version load comparison (phase 2).
//
// Like api_benchmark.cpp this compiles once per agent version through
// pp_compat.h; unlike it, the workload arrives over HTTP from
// test/e2e/fixed_rps_test.py / max_throughput_test.py, so what is measured is
// a served request including the agent's instrumentation, not a bare API call.
//
// The full e2e server (test/e2e/e2e_server.cpp) is not reused because it is
// written against the current API and exercises version-specific surfaces
// (C API, gRPC downstreams, agent lifecycle endpoints). This server implements
// just the load scripts' endpoint contract on the version-neutral shim:
// /simple /deep /wide /annotated /features /mixed /error /db-* plus the
// /stats and /ready pre-flight endpoints.
//
// --disable starts the agent with `Enable: false`: every handler still makes
// the same API calls but gets noop spans, which is the baseline that separates
// harness cost from agent overhead.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"
#include "pp_compat.h"

namespace {

    std::atomic<uint64_t> g_total_requests{0};
    std::atomic<int64_t> g_active_requests{0};
    std::atomic<bool> g_disabled_mode{false};

    ppc::AgentPtr g_agent;

    constexpr int32_t kServiceType = pinpoint::SERVICE_TYPE_CPP;
    constexpr int32_t kMethodType = pinpoint::SERVICE_TYPE_CPP_FUNC;
    constexpr int32_t kSqlType = pinpoint::SERVICE_TYPE_MYSQL_QUERY;

    struct RequestGuard {
        RequestGuard() {
            g_total_requests.fetch_add(1, std::memory_order_relaxed);
            g_active_requests.fetch_add(1, std::memory_order_relaxed);
        }
        ~RequestGuard() { g_active_requests.fetch_sub(1, std::memory_order_relaxed); }
    };

    // Copies the request headers into the shim reader once per request. Both
    // versions pay the identical copy; extraction itself then runs through
    // whichever Get() signature the version exposes.
    ppc::VectorHeaderReader make_reader(const httplib::Request& req) {
        std::vector<std::pair<std::string, std::string>> headers;
        headers.reserve(req.headers.size());
        for (const auto& [key, value] : req.headers) {
            headers.emplace_back(key, value);
        }
        return ppc::VectorHeaderReader(std::move(headers));
    }

    ppc::SpanPtr new_span(const httplib::Request& req, const char* rpc) {
        auto reader = make_reader(req);
        auto span = g_agent->NewSpan("E2E-Bench", rpc, req.method, reader);
        span->SetServiceType(kServiceType);
        span->SetRemoteAddress(req.remote_addr);
        span->SetEndPoint("127.0.0.1:8090");
        return span;
    }

    void finish(const ppc::SpanPtr& span, httplib::Response& resp, int status,
                const char* body = "{\"ok\":true}") {
        span->SetStatusCode(status);
        span->EndSpan();
        resp.status = status;
        resp.set_content(body, "application/json");
    }

    int query_int(const httplib::Request& req, const char* key, int fallback,
                  int min_value, int max_value) {
        if (!req.has_param(key)) {
            return fallback;
        }
        int value = fallback;
        try {
            value = std::stoi(req.get_param_value(key));
        } catch (...) {
            return fallback;
        }
        return std::min(std::max(value, min_value), max_value);
    }

    // ---- workload endpoints ------------------------------------------------

    void on_simple(const httplib::Request& req, httplib::Response& resp) {
        RequestGuard guard;
        auto span = new_span(req, "/simple");
        auto event = span->NewSpanEvent("SimpleService.handle()", kMethodType);
        ppc::EndEvent(span, event);
        finish(span, resp, 200);
    }

    void on_deep(const httplib::Request& req, httplib::Response& resp) {
        RequestGuard guard;
        const int depth = query_int(req, "depth", 10, 1, 64);
        auto span = new_span(req, "/deep");
        std::vector<ppc::EventPtr> stack;
        stack.reserve(static_cast<size_t>(depth));
        for (int i = 0; i < depth; ++i) {
            stack.push_back(span->NewSpanEvent("Deep.level()", kMethodType));
        }
        for (int i = depth; i > 0; --i) {
            ppc::EndEvent(span, stack[static_cast<size_t>(i - 1)]);
        }
        finish(span, resp, 200);
    }

    void on_wide(const httplib::Request& req, httplib::Response& resp) {
        RequestGuard guard;
        const int width = query_int(req, "width", 20, 1, 512);
        auto span = new_span(req, "/wide");
        for (int i = 0; i < width; ++i) {
            auto event = span->NewSpanEvent("Wide.step()", kMethodType);
            ppc::EndEvent(span, event);
        }
        finish(span, resp, 200);
    }

    void on_annotated(const httplib::Request& req, httplib::Response& resp) {
        RequestGuard guard;
        auto span = new_span(req, "/annotated");
        ppc::AnnotateString(span, pinpoint::ANNOTATION_HTTP_URL, "/annotated?page=3");
        ppc::AnnotateInt(span, pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);

        auto event = span->NewSpanEvent("Annotated.decorate()", kMethodType);
        ppc::AnnotateString(event, pinpoint::ANNOTATION_API, "Annotated.decorate(String, int)");
        ppc::AnnotateLong(event, pinpoint::ANNOTATION_API, 1700000000000LL);
        ppc::AnnotateStringString(event, pinpoint::ANNOTATION_HTTP_REQUEST_HEADER,
                                  "page", "3");
        ppc::AnnotateStringString(event, pinpoint::ANNOTATION_HTTP_REQUEST_HEADER,
                                  "sort", "created_at:desc");
        ppc::EndEvent(span, event);
        finish(span, resp, 200);
    }

    const std::string kCrudStatements[4] = {
        "INSERT INTO orders (customer_id, total, status) VALUES (?, ?, ?)",
        "SELECT o.id, o.total FROM orders o WHERE o.customer_id = ? AND o.status = ?",
        "UPDATE orders SET status = ? WHERE id = ?",
        "DELETE FROM order_drafts WHERE customer_id = ? AND created_at < ?",
    };
    const std::string kComplexStatement =
        "SELECT o.id, o.total, c.name, SUM(i.qty * i.price) AS amount "
        "FROM orders o JOIN customers c ON c.id = o.customer_id "
        "JOIN order_items i ON i.order_id = o.id "
        "WHERE o.status = ? AND o.created_at BETWEEN ? AND ? "
        "GROUP BY o.id, o.total, c.name HAVING amount > ? "
        "ORDER BY amount DESC LIMIT 50";
    const std::vector<std::string> kBinds{"1024", "SHIPPED", "99.50"};

    void sql_event(const ppc::SpanPtr& span, const std::string& statement,
                   const std::vector<std::string>& binds) {
        auto event = span->NewSpanEvent("OrderRepository.query()", kSqlType);
        event->SetDestination("orders-db");
        event->SetEndPoint("mysql.local:3306");
        ppc::SetSqlQuery(event, statement, binds);
        ppc::EndEvent(span, event);
    }

    void on_features(const httplib::Request& req, httplib::Response& resp) {
        RequestGuard guard;
        auto span = new_span(req, "/features");

        auto step = span->NewSpanEvent("Features.step()", kMethodType);
        ppc::AnnotateString(step, pinpoint::ANNOTATION_HTTP_URL, "/features");
        ppc::EndEvent(span, step);

        sql_event(span, kCrudStatements[1], kBinds);

        // Async child used on the same thread: supported by both versions and
        // exercises the async-span bookkeeping without a second thread.
        auto async_span = span->NewAsyncSpan("Features.async");
        auto async_event = async_span->NewSpanEvent("Features.asyncStep()", kMethodType);
        ppc::EndEvent(async_span, async_event);
        async_span->EndSpan();

        const auto trace_id = ppc::TraceIdString(span);
        resp.set_header("X-Bench-Trace", trace_id);
        finish(span, resp, 200);
    }

    void on_mixed(const httplib::Request& req, httplib::Response& resp) {
        RequestGuard guard;
        auto span = new_span(req, "/mixed");
        auto event = span->NewSpanEvent("Mixed.handle()", kMethodType);
        ppc::AnnotateInt(event, pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);
        ppc::EndEvent(span, event);
        sql_event(span, kCrudStatements[1], kBinds);
        finish(span, resp, 200);
    }

    void on_error(const httplib::Request& req, httplib::Response& resp) {
        RequestGuard guard;
        auto span = new_span(req, "/error");
        auto event = span->NewSpanEvent("Error.fail()", kMethodType);
        event->SetError("bench-error", "synthetic failure for the error workload");
        ppc::EndEvent(span, event);
        span->SetError("bench-error");
        finish(span, resp, 500, "{\"ok\":false}");
    }

    void on_db_crud(const httplib::Request& req, httplib::Response& resp) {
        RequestGuard guard;
        auto span = new_span(req, "/db-crud");
        for (const auto& statement : kCrudStatements) {
            sql_event(span, statement, kBinds);
        }
        finish(span, resp, 200);
    }

    void on_db_batch(const httplib::Request& req, httplib::Response& resp) {
        RequestGuard guard;
        const int size = query_int(req, "size", 10, 1, 256);
        auto span = new_span(req, "/db-batch");
        for (int i = 0; i < size; ++i) {
            sql_event(span, kCrudStatements[0], kBinds);
        }
        finish(span, resp, 200);
    }

    void on_db_complex(const httplib::Request& req, httplib::Response& resp) {
        RequestGuard guard;
        auto span = new_span(req, "/db-complex");
        std::vector<std::string> binds{"SHIPPED", "2026-01-01", "2026-02-01", "500.0"};
        sql_event(span, kComplexStatement, binds);
        finish(span, resp, 200);
    }

    // ---- control endpoints ---------------------------------------------------

    void on_stats(const httplib::Request&, httplib::Response& resp) {
        char body[160];
        std::snprintf(body, sizeof(body),
                      "{\"total_requests\":%llu,\"active_requests\":%lld}",
                      static_cast<unsigned long long>(g_total_requests.load(std::memory_order_relaxed)),
                      static_cast<long long>(g_active_requests.load(std::memory_order_relaxed)));
        resp.status = 200;
        resp.set_content(body, "application/json");
    }

    void on_ready(const httplib::Request&, httplib::Response& resp) {
        if (g_disabled_mode.load()) {
            resp.status = 200;
            resp.set_content("{\"agent\":\"disabled\"}", "application/json");
            return;
        }
        if (g_agent != nullptr && g_agent->Enable()) {
            resp.status = 200;
            resp.set_content("{\"agent\":\"enabled\"}", "application/json");
            return;
        }
        resp.status = 503;
        resp.set_content("{\"agent\":\"starting\"}", "application/json");
    }

    std::string build_config(const std::string& host, int agent_port, int span_port,
                             int stat_port, bool disabled) {
        // Same key spelling rationale as api_benchmark.cpp: Collector.Grpc*
        // works verbatim in both versions, and Span.QueueSize is capped at
        // 65536 in both.
        std::string config =
            "ApplicationName: cpp-load-compare\n";
        if (disabled) {
            config += "Enable: false\n";
        }
        config +=
            "Collector:\n"
            "  GrpcHost: " + host + "\n"
            "  GrpcAgentPort: " + std::to_string(agent_port) + "\n"
            "  GrpcSpanPort: " + std::to_string(span_port) + "\n"
            "  GrpcStatPort: " + std::to_string(stat_port) + "\n"
            "Sampling:\n"
            "  Type: COUNTER\n"
            "  CounterRate: 1\n"
            "Span:\n"
            "  QueueSize: 65536\n"
            "Stat:\n"
            "  Enable: false\n"
            "Log:\n"
            "  Level: error\n";
        return config;
    }

}  // namespace

int main(int argc, char** argv) {
    int port = 8090;
    int pool_size = 8;
    std::string collector_host = "127.0.0.1";
    int agent_port = 9991;
    int span_port = 9993;
    int stat_port = 9992;
    bool disabled = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string{}; };
        if (arg == "--port") {
            port = std::stoi(next());
        } else if (arg == "--pool") {
            pool_size = std::stoi(next());
        } else if (arg == "--host") {
            collector_host = next();
        } else if (arg == "--agent-port") {
            agent_port = std::stoi(next());
        } else if (arg == "--span-port") {
            span_port = std::stoi(next());
        } else if (arg == "--stat-port") {
            stat_port = std::stoi(next());
        } else if (arg == "--disable") {
            disabled = true;
        }
    }

    g_disabled_mode.store(disabled);
    g_agent = ppc::StartAgent(
        build_config(collector_host, agent_port, span_port, stat_port, disabled));
    if (g_agent == nullptr) {
        // v1.1.0's CreateAgent never returns null; the current StartAgent does
        // when Enable is false. The noop GlobalAgent still serves the API.
        g_agent = pinpoint::GlobalAgent();
    }
    if (!disabled) {
        if (!ppc::WaitUntilEnabled(g_agent, std::chrono::seconds(30))) {
            std::cerr << "[server " << ppc::kApiVariant
                      << "] agent never became ready; is the collector running?" << std::endl;
            return 1;
        }
    }
    std::cerr << "[server " << ppc::kApiVariant << "] agent "
              << (disabled ? "disabled (baseline mode)" : "enabled") << std::endl;

    httplib::Server server;
    server.new_task_queue = [pool_size] { return new httplib::ThreadPool(static_cast<size_t>(pool_size)); };

    server.Get("/simple", on_simple);
    server.Get("/deep", on_deep);
    server.Get("/wide", on_wide);
    server.Get("/annotated", on_annotated);
    server.Get("/features", on_features);
    server.Get("/mixed", on_mixed);
    server.Get("/error", on_error);
    server.Get("/db-crud", on_db_crud);
    server.Get("/db-batch", on_db_batch);
    server.Get("/db-complex", on_db_complex);
    server.Get("/stats", on_stats);
    server.Get("/ready", on_ready);

    std::cerr << "[server " << ppc::kApiVariant << "] listening on 127.0.0.1:" << port
              << " pool=" << pool_size << std::endl;
    // Runs until killed by the orchestrator; agent shutdown is deliberately
    // left to process exit so a run's teardown cannot affect the next sample.
    if (!server.listen("127.0.0.1", port)) {
        std::cerr << "[server " << ppc::kApiVariant << "] failed to bind port " << port << std::endl;
        return 1;
    }
    return 0;
}
