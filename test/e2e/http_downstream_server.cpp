#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "httplib.h"
#include "http_trace_context.h"
#include "pinpoint/tracer.h"

#include "e2e_common.h"

namespace {

pinpoint::SpanPtr make_span(const httplib::Request& req) {
    auto agent = pinpoint::GlobalAgent();
    HttpHeaderReader reader(req.headers);
    auto span = agent->NewSpan("it-http-downstream", req.path, req.method, reader);
    pinpoint::helper::TraceHttpServerRequest(
        span, req.remote_addr, req.get_header_value("Host"), reader);
    return span;
}

void health(const httplib::Request&, httplib::Response& res) {
    auto agent = pinpoint::GlobalAgent();
    std::ostringstream body;
    body << "{\"agent_enabled\":" << (agent->Enable() ? "true" : "false")
         << ",\"collector_host\":\""
         << it_test::env_or("PINPOINT_CPP_COLLECTOR_HOST", "") << "\"}";
    res.set_content(body.str(), "application/json");
}

void trace(const httplib::Request& req, httplib::Response& res) {
    auto span = make_span(req);
    const bool failed = req.path == "/error";
    auto event = span->NewSpanEvent("downstream_work");
    event->SetServiceType(pinpoint::SERVICE_TYPE_CPP_FUNC);
    event->SetDestination("http-downstream");
    event->EndEvent();

    const int status = failed ? 503 : 200;
    span->SetStatusCode(status);
    span->SetUrlStat(req.path, req.method, status);
    res.status = status;
    res.set_header("X-It-Trace-Id", span->GetTraceId());
    res.set_header("X-It-Span-Id", std::to_string(span->GetSpanId()));
    std::ostringstream body;
    body << "{\"trace_id\":\"" << span->GetTraceId()
         << "\",\"span_id\":" << span->GetSpanId()
         << ",\"sampled\":" << (span->IsSampled() ? "true" : "false")
         << ",\"incoming_trace_id\":\""
         << req.get_header_value(std::string(pinpoint::HEADER_TRACE_ID))
         << "\",\"incoming_span_id\":\""
         << req.get_header_value(std::string(pinpoint::HEADER_SPAN_ID))
         << "\",\"incoming_parent_span_id\":\""
         << req.get_header_value(std::string(pinpoint::HEADER_PARENT_SPAN_ID))
         << "\",\"incoming_sampled\":\""
         << req.get_header_value(std::string(pinpoint::HEADER_SAMPLED))
         << "\",\"trace_id_matches\":"
         << ((req.get_header_value(std::string(pinpoint::HEADER_TRACE_ID)) == span->GetTraceId()) ? "true" : "false")
         << ",\"span_id_matches\":"
         << (!req.get_header_value(std::string(pinpoint::HEADER_SPAN_ID)).empty() ? "true" : "false")
         << "}";
    res.set_content(body.str(), "application/json");
    HttpHeaderReader response_reader(res.headers);
    span->RecordHeader(pinpoint::HTTP_RESPONSE, response_reader);
    span->EndSpan();
}

}  // namespace

int main(int argc, char** argv) {
    // Redirected stdout is block-buffered, and the process then blocks serving
    // requests until it is killed, so run_e2e.sh's log greps can race the
    // unflushed tail. Matches e2e_server.cpp.
    setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);

    const int port = argc > 1 ? std::atoi(argv[1]) : 8091;
    it_test::ConfigureAgentEnvironment("cpp-it-http-downstream",
                                       "it-http-downstream");
    if (!pinpoint::StartAgent()) {
        std::cerr << "pinpoint agent start failed; check the agent log" << std::endl;
    }
    auto agent = pinpoint::GlobalAgent();

    httplib::Server server;
    server.Get("/health", health);
    server.Get("/echo", trace);
    server.Get("/trace", trace);
    server.Get("/error", trace);
    // run_e2e.sh posts this before falling back to SIGTERM. Without the route
    // the process only ever died from the signal, which skips the exit
    // handlers — including the one that writes the coverage profile, so this
    // server contributed nothing to a coverage run. Mirrors
    // e2e_server.cpp's /server/shutdown.
    server.Post("/shutdown", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"status\":\"shutting_down\"}", "application/json");
        std::thread([&server]() { server.stop(); }).detach();
    });
    if (!server.listen("0.0.0.0", port)) {
        return 2;
    }
    agent->Shutdown();
    return 0;
}
