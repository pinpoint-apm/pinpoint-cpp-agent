#include <iostream>
#include <thread>
#include <random>
#include <string>

#include "pinpoint/tracer.h"

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

static int random_number2() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, 100);
    return dis(gen);
}

static std::string make_func_name() {
    return "func_" + std::to_string(random_number2());
}


//static void worker(const pinpoint::AgentPtr& agent) {
static void worker() {
    auto count = 0;
    while (true) {
        if (count++; count > 1000) {
            break;
        }

        const auto rand_url = random_number();
        const auto rand_method = random_number();
        const auto rand_status = random_number();

        auto& path = urls[rand_url];
        const auto agent = pinpoint::GlobalAgent();
        const auto span = agent->NewSpan("C++ Http Server", path);

        span->SetRemoteAddress("192.168.1.1");
        span->SetEndPoint("127.0.0.1:8080");

        span->NewSpanEvent(make_func_name());
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
        span->NewSpanEvent(make_func_name());
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));

        span->NewSpanEvent(make_func_name());
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
        span->EndSpanEvent();

        span->NewSpanEvent(make_func_name());
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
        span->NewSpanEvent(make_func_name());
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
        span->NewSpanEvent(make_func_name());
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
        span->EndSpanEvent();
        span->EndSpanEvent();
        span->EndSpanEvent();

        span->NewSpanEvent(make_func_name());
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
        span->NewSpanEvent(make_func_name());
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
        span->NewSpanEvent(make_func_name());
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
        span->NewSpanEvent(make_func_name());
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
        span->NewSpanEvent(make_func_name());
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
        span->NewSpanEvent(make_func_name());
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time[random_number()]));
        span->EndSpanEvent();
        span->EndSpanEvent();
        span->EndSpanEvent();
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

        const auto status_code = http_status[rand_status];
        span->SetStatusCode(status_code);
        span->SetUrlStat(path, methods[rand_method], status_code);
        span->EndSpan();
    }
}

int main() {
    setenv("PINPOINT_CPP_CONFIG_FILE", "/tmp/pinpoint-config.yaml", 0);
    setenv("PINPOINT_CPP_APPLICATION_NAME", "cpp-url_stat_test", 0);
    setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true", 0);
    setenv("PINPOINT_CPP_SAMPLING_COUNTER_RATE", "2", 0);
    setenv("PINPOINT_CPP_LOG_LEVEL", "debug", 0);
    setenv("PINPOINT_CPP_LOG_FILE_PATH", "/tmp/pinpoint.log", 0);

    // auto agent = pinpoint::CreateAgent();
    // std::this_thread::sleep_for(std::chrono::seconds(1));

    std::thread threads[20];
    for (auto& th : threads) {
        // th = std::thread{&worker, agent};
        th = std::thread{&worker};
    }

    std::this_thread::sleep_for(std::chrono::seconds(5));
    auto agent = pinpoint::CreateAgent();

    std::this_thread::sleep_for(std::chrono::seconds(300));
    agent->Shutdown();

    for (auto& th : threads) {
        th.join();
    }

    // std::this_thread::sleep_for(std::chrono::seconds(5));
    // agent->Shutdown();
    return 0;
}
