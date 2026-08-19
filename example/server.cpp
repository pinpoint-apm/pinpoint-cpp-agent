/**
 * @file server.cpp
 * @brief Backend app for the distributed tracing demo (see README.md).
 *
 * Shows the server-side tracing surface of the C++ agent in one request flow:
 *
 *   GET /api/members
 *     → new span with the trace context extracted from the request headers,
 *       making it a child of the proxy's span in the same trace
 *       (helper::TraceHttpServerRequest also records the remote address,
 *        the endpoint, and the configured request headers — here User-Agent)
 *     → (simulated) MySQL query traced as a child span event
 *        · SERVICE_TYPE_MYSQL_QUERY + destination/endpoint of the database
 *          make the database its own node in the server map
 *        · SetSqlQuery records the statement itself
 *     → follow-up work handed to a worker thread traced with an async span
 *        · created on the request thread, used only by the worker thread
 *     → response recorded via helper::TraceHttpServerResponse
 *       (status code, URL stats, configured response headers), span ended
 *
 * The span travels to the traced helpers through a thread_local slot instead
 * of parameters, and span events use the RAII helper::ScopedSpanEvent.
 */
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "pinpoint/tracer.h"
#include "httplib.h"
#include "http_trace_context.h"

// Span of the request being handled on this thread. The handler stores it
// here so the traced helpers below pick it up without parameter plumbing;
// per-thread storage also keeps each span on its single owning thread, as
// the Span thread-safety contract requires.
static thread_local pinpoint::SpanPtr t_span;

/* ---- Root span: extract inbound trace context --------------------------- */

// Opens the root span for an inbound request. NewSpan() reads the Pinpoint-*
// propagation headers through the reader, so this span continues the trace
// started by the caller (the proxy). TraceHttpServerRequest then records the
// remote address, the endpoint, and the configured request headers.
static pinpoint::SpanPtr make_span(const httplib::Request& req) {
    HttpHeaderReader header_reader(req.headers);
    auto span = pinpoint::GlobalAgent()->NewSpan("C++ DB Server", req.path, req.method, header_reader);

    auto end_point = req.get_header_value("Host");
    if (end_point.empty()) {
        end_point = req.local_addr + ":" + std::to_string(req.local_port);
    }
    pinpoint::helper::TraceHttpServerRequest(span, req.remote_addr, end_point, header_reader);
    return span;
}

/* ---- DB query traced as a MySQL span event ------------------------------ */

// Simulates executing the members query and returns the result rows as JSON.
//
// This is a stand-in so the demo needs no real database. In a real
// application, connect to the database and run the query right here —
// the surrounding span event in query_members() traces that work as-is;
// nothing about the tracing changes.
static std::string execute_query_simulated() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return "{\"members\":[{\"id\":1,\"name\":\"pinpoint\"},"
           "{\"id\":2,\"name\":\"naver\"},{\"id\":3,\"name\":\"cpp-agent\"}]}";
}

// Answers the request with the member rows, tracing the (simulated) query
// as a child span event on the thread-local span.
static void query_members(httplib::Response& res) {
    static const char* kQuery = "SELECT id, name FROM members";

    // RAII span event: EndEvent() runs when the scope closes. The MySQL
    // service type plus destination/endpoint make the database show up as
    // its own node in the server map; SetSqlQuery records the statement.
    // On a query failure, record it with event->SetError(...).
    pinpoint::helper::ScopedSpanEvent event(t_span, "members.select",
                                            pinpoint::SERVICE_TYPE_MYSQL_QUERY);
    event->SetDestination("demo");
    event->SetEndPoint("mysql:3306");
    event->SetSqlQuery(kQuery, {});

    res.status = 200;
    res.set_content(execute_query_simulated(), "application/json");
}

/* ---- Background work traced with an async span -------------------------- */

// Hands follow-up work to a worker thread traced with an async span.
static void run_async_audit() {
    // NewAsyncSpan() hangs the async child off the open span event, so this
    // scoped event must still be alive when it is called.
    pinpoint::helper::ScopedSpanEvent schedule_event(t_span, "audit.schedule");

    // The async span is created on this (owning) thread and then used
    // exclusively by the worker — one span instance must only ever be used
    // by a single thread. It is handed over by capture: thread-local state
    // does not cross threads.
    auto async_span = t_span->NewAsyncSpan("audit.async");
    std::thread async_worker([async_span] {
        {
            // The worker's own work, as an event on the async span; the
            // scope ends the event before the async span is ended below.
            pinpoint::helper::ScopedSpanEvent work(async_span, "audit.write");
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        async_span->EndSpan();
    });
    // Joined so the tracing thread never races agent shutdown.
    async_worker.join();
}

/* ---- Entry point --------------------------------------------------------- */

int main() {
    setenv("PINPOINT_CPP_APPLICATION_NAME", "cpp-db-server", 0);
    setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true", 0);
    // Record only the User-Agent request header on the span (see doc/config.md).
    setenv("PINPOINT_CPP_HTTP_SERVER_RECORD_REQUEST_HEADER", "User-Agent", 0);

    if (!pinpoint::StartAgent()) {
        std::cerr << "failed to start the pinpoint agent: check the agent log" << std::endl;
    }

    httplib::Server server;
    server.Get("/api/members", [](const httplib::Request& req, httplib::Response& res) {
        // 1. Root span from the inbound trace context, published to the
        //    traced helpers through the thread-local slot.
        auto span = make_span(req);
        t_span = span;

        // 2. Traced work, wrapped in one handler-level span event whose
        //    scope covers the whole handler body; the events opened inside
        //    become its children in the call tree. Both helpers read the
        //    span from t_span.
        {
            pinpoint::helper::ScopedSpanEvent handler_event(span, "server.members");
            query_members(res);
            run_async_audit();
        }

        // 3. Record the response (status code, URL stats, configured
        //    response headers) and finish the span; the recorded data is
        //    delivered to the collector asynchronously.
        HttpHeaderReader response_reader(res.headers);
        pinpoint::helper::TraceHttpServerResponse(span, req.path, req.method, res.status, response_reader);
        span->EndSpan();
        // httplib pools threads: clear the slot so the span is released now
        // and never seen by the next request on this thread.
        t_span.reset();
    });

    std::cout << "db server listening on :8081" << std::endl;
    server.listen("0.0.0.0", 8081);
    pinpoint::GlobalAgent()->Shutdown();
}
