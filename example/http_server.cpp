#include <random>
#include <string>

#include "pinpoint/tracer.h"
#include "httplib.h"
#include "http_trace_context.h"

pinpoint::SpanPtr make_span(const httplib::Request& req);
void on_foo(const httplib::Request& request, httplib::Response& response);


int main() {
    setenv("PINPOINT_CPP_CONFIG_FILE", "/tmp/pinpoint-config.yaml", 0);
    setenv("PINPOINT_CPP_APPLICATION_NAME", "cpp-http-server", 0);
    setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true", 0);
    //setenv("PINPOINT_CPP_LOG_LEVEL", "debug", 0);

    auto agent = pinpoint::CreateAgent();

    httplib::Server server;
    server.Get("/foo", on_foo);

    server.listen("0.0.0.0", 8090);
    agent->Shutdown();
}

pinpoint::SpanPtr make_span(const httplib::Request& req) {
    auto agent = pinpoint::GlobalAgent();

    HttpHeaderReader h_reader(req.headers); 
    auto span = agent->NewSpan("C++ Http Server", req.path, h_reader);

    auto end_point = req.get_header_value("Host");
    if (end_point.empty()) {
        end_point = req.local_addr + ":" + std::to_string(req.local_port);
    }
    pinpoint::helper::TraceHttpServerRequest(span, req.remote_addr, end_point, h_reader);

    return span;
}

static int http_status[5] = {200, 303, 404, 500, 501};
static std::string urls[5] = {"/path/to?resource=here",
                              "/example/to?resource=here",
                              "/pinpoint",
                              "/pinpoint-apm/pinpoint",
                              "/pinpoint-envoy/to?resource=here"};
static std::string methods[5] = {"GET",
                              "PUT",
                              "DELETE",
                              "GET",
                              "PUT"};
static int sleep_time[5] = {5, 10, 50, 80, 100};

static int random_number() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, 4);
    return dis(gen);
}

static void f(const pinpoint::SpanPtr& span) {
    const auto rand_url = random_number();
    const auto rand_method = random_number();
    const auto rand_status = random_number();

    span->NewSpanEvent("func_example");
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
    span->NewSpanEvent("func_1");
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));

    span->NewSpanEvent("func_2");
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
    span->EndSpanEvent();

    span->NewSpanEvent("func_3");
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
    span->NewSpanEvent("func_4");
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
    span->NewSpanEvent("func_5");
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
    span->EndSpanEvent();
    span->EndSpanEvent();
    span->EndSpanEvent();

    span->NewSpanEvent("foo");
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
    span->NewSpanEvent("bar");
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
    span->EndSpanEvent();
    span->EndSpanEvent();

    span->EndSpanEvent();
    span->EndSpanEvent();

    auto& path = urls[rand_url];
    const auto status_code = http_status[rand_status];
    span->SetStatusCode(status_code);
    span->SetUrlStat(path, methods[rand_method], status_code);
}

void on_foo(const httplib::Request& req, httplib::Response& res) {
    auto span = make_span(req);
    span->NewSpanEvent("foo");

    std::random_device rd;
    std::uniform_int_distribution<int> distribution(10, 500);
    std::this_thread::sleep_for(std::chrono::milliseconds(distribution(rd)));

    res.set_content("hello, foo!!", "text/plain");
    span->EndSpanEvent();

    HttpHeaderReader http_reader(res.headers);
    span->RecordHeader(pinpoint::HTTP_RESPONSE, http_reader);

    f(span);

    span->EndSpan();
}
