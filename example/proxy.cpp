/**
 * @file proxy.cpp
 * @brief Proxy app for the distributed tracing demo (see README.md).
 *
 * Shows client-side tracing and cross-process context propagation in one
 * request flow, so the whole chain becomes a single distributed trace:
 *
 *   client → cpp-proxy → cpp-db-server → MySQL
 *
 *   GET /api/members
 *     → new span for the inbound request
 *       (helper::TraceHttpServerRequest records the remote address, the
 *        endpoint, and the configured request headers — here User-Agent)
 *     → outbound call to the backend traced as a child span event
 *        · helper::TraceHttpClientRequest sets SERVICE_TYPE_CPP_HTTP_CLIENT,
 *          destination/endpoint, the URL annotation, and records the
 *          configured outbound request headers
 *        · InjectContext writes the Pinpoint-* propagation headers into the
 *          outbound request — the backend continues this trace from them
 *        · helper::TraceHttpClientResponse records the downstream status
 *        · a failed call is recorded as an error on the event and the span
 *     → response recorded via helper::TraceHttpServerResponse
 *       (status code, URL stats, configured response headers), span ended
 */
#include <cstdlib>
#include <iostream>
#include <string>

#include "pinpoint/tracer.h"
#include "httplib.h"
#include "http_trace_context.h"

static std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return (value && *value) ? value : fallback;
}

int main() {
    setenv("PINPOINT_CPP_APPLICATION_NAME", "cpp-proxy", 0);
    //setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true", 0);

    // Record only the User-Agent request header, on the span for incoming
    // requests and on the client span event for outgoing ones (see doc/config.md).
    setenv("PINPOINT_CPP_HTTP_SERVER_RECORD_REQUEST_HEADER", "User-Agent", 0);

    if (!pinpoint::StartAgent()) {
        std::cerr << "failed to start the pinpoint agent: check the agent log" << std::endl;
    }

    const std::string backend = env_or("BACKEND", "127.0.0.1:8081");

    httplib::Server server;
    server.Get("/api/members", [&backend](const httplib::Request& req, httplib::Response& res) {
        // 1. Root span for the inbound request.
        //    Opens the root span for an inbound request. NewSpan() reads any Pinpoint-*
        //    propagation headers through the reader (a direct client call starts a new
        //    trace); TraceHttpServerRequest then records the remote address, the
        //    endpoint, and the configured request headers.
        HttpHeaderReader header_reader(req.headers);
        auto span = pinpoint::GlobalAgent()->NewSpan("C++ Proxy", req.path, req.method, header_reader);
        pinpoint::helper::TraceHttpServerRequest(span, req.remote_addr, "proxy:8080", header_reader);

        // 2. The outbound call is a child span event. TraceHttpClientRequest
        //    marks it as an HTTP client call to `backend`; InjectContext
        //    writes this event's trace context into the outbound headers so
        //    the backend's span becomes its child.
        auto event = span->NewSpanEvent("proxy.forward");

        httplib::Headers headers = {{"Accept", "application/json"},
                                    {"User-Agent", "pinpoint-cpp-demo-proxy"}};
        HttpHeaderReaderWriter header_writer(headers);
        pinpoint::helper::TraceHttpClientRequest(
            event, backend, "http://" + backend + req.path, header_writer);
        event->InjectContext(header_writer);

        httplib::Client client(backend);
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(5, 0);

        // 3. Record the downstream result on the client event: the status
        //    code on success, or the failure on both the event and the span
        //    so the transaction is marked failed with its cause.
        if (auto result = client.Get(req.path, headers)) {
            HttpHeaderReader response_reader(result->headers);
            pinpoint::helper::TraceHttpClientResponse(event, result->status, response_reader);
            res.status = result->status;
            res.set_content(result->body, result->get_header_value("Content-Type", "application/json"));
        } else {
            const auto error = httplib::to_string(result.error());
            event->SetError("HttpClientError", error);
            span->SetError("HttpClientError", error);
            res.status = 502;
            res.set_content("{\"error\":\"backend unreachable: " + error + "\"}", "application/json");
        }
        event->EndEvent();

        // 4. Record the response (status code, URL stats, configured
        //    response headers) and finish the span; the recorded data is
        //    delivered to the collector asynchronously.
        HttpHeaderReader response_reader(res.headers);
        pinpoint::helper::TraceHttpServerResponse(span, req.path, req.method, res.status, response_reader);
        span->EndSpan();
    });

    std::cout << "proxy listening on :8080, forwarding to " << backend << std::endl;
    server.listen("0.0.0.0", 8080);
    pinpoint::GlobalAgent()->Shutdown();
}
