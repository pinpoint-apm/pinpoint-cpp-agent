#include <iostream>
#include <thread>

#include "pinpoint/tracer.h"
#include "httplib.h"
#include "http_trace_context.h"

int main() {
    setenv("PINPOINT_CPP_CONFIG_FILE", "/tmp/pinpoint-config.yaml", 0);
    setenv("PINPOINT_CPP_APPLICATION_NAME", "cpp-tutorial", 0);

    auto agent = pinpoint::CreateAgent();
    std::this_thread::sleep_for(std::chrono::seconds(5));

    httplib::Server svr;
    svr.Get("/foo", [&](const httplib::Request& req, httplib::Response& resp) {
        HttpTraceContextReader trace_context_reader(req.headers);
        auto span = agent->NewSpan("C++ Web Server", "/foo", trace_context_reader);

        span->SetRemoteAddress(req.remote_addr);
        auto end_point = req.get_header_value("Host");
        if (end_point.empty()) {
            end_point = req.local_addr + ":" + std::to_string(req.local_port);
        }
        span->SetEndPoint(end_point);

        HttpHeaderReader http_reader(req.headers);
        span->RecordHeader(pinpoint::HTTP_REQUEST, http_reader);

        auto host = "localhost:8090";
	    auto se = span->NewSpanEvent("TestSpanEvent");
        se->SetServiceType(pinpoint::SERVICE_TYPE_CPP_HTTP_CLIENT);
        se->SetEndPoint(host);
        se->SetDestination(host);

        httplib::Headers headers;
        HttpTraceContextWriter trace_context_writer(headers);
        span->InjectContext(trace_context_writer);

        auto anno = se->GetAnnotations();
        anno->AppendString(pinpoint::ANNOTATION_HTTP_URL, "localhost:8090/foo");

        httplib::Client cli(host);
        auto res = cli.Get("/foo", headers);
        if (res) {
            if (res->status == httplib::OK_200) {
                std::cout << res->body << std::endl;
            }

            anno->AppendInt(pinpoint::ANNOTATION_HTTP_STATUS_CODE, res->status);
        } else {
            auto err = res.error();
            auto err_msg = httplib::to_string(err);
            std::cout << "HTTP error: " << err_msg << std::endl;
            se->SetError(err_msg);
            span->SetError("http client error");
        }

        span->EndSpanEvent();

        span->NewSpanEvent("TestSpanEvent2");
        auto async_span = span->NewAsyncSpan("New Thread");
        async_span->NewSpanEvent("ThreadSpanEvent");
        async_span->EndSpanEvent();
        async_span->EndSpan();
        span->EndSpanEvent();

        span->SetStatusCode(200);
        span->EndSpan();

        resp.set_content(req.body, "text/plain");
    });
    svr.listen("0.0.0.0", 8080);

    std::this_thread::sleep_for(std::chrono::seconds(600));
    agent->Shutdown();
    return 0;
}
