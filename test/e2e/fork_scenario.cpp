#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "e2e_common.h"
#include "pinpoint/tracer.h"

namespace {

bool WaitUntilEnabled(const pinpoint::AgentPtr& agent) {
    const auto* timeout_value = std::getenv("PINPOINT_IT_AGENT_TIMEOUT");
    const int timeout = timeout_value == nullptr ? 30 : std::atoi(timeout_value);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(timeout);
    while (std::chrono::steady_clock::now() < deadline) {
        if (agent->Enable()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return agent->Enable();
}

std::string RunChild(const pinpoint::AgentPtr& cold_agent, int index,
                     int* child_status) {
    int fds[2];
    if (pipe(fds) != 0) {
        *child_status = 1;
        return {};
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        *child_status = 1;
        return {};
    }

    if (pid == 0) {
        close(fds[0]);
        cold_agent->Start();
        if (!WaitUntilEnabled(cold_agent)) {
            const char* failure = "collector-timeout";
            (void)write(fds[1], failure, std::strlen(failure));
            close(fds[1]);
            _exit(2);
        }

        auto span = cold_agent->NewSpan(
            "fork-child", "/fork/" + std::to_string(index));
        const std::string trace_id = span->GetTraceId();
        span->NewSpanEvent("fork-child-work")->EndEvent();
        span->EndSpan();
        (void)write(fds[1], trace_id.data(), trace_id.size());
        close(fds[1]);
        cold_agent->Shutdown();
        _exit(trace_id.empty() ? 3 : 0);
    }

    close(fds[1]);
    std::string trace_id;
    char buffer[256];
    ssize_t count;
    while ((count = read(fds[0], buffer, sizeof(buffer))) > 0) {
        trace_id.append(buffer, static_cast<size_t>(count));
    }
    close(fds[0]);
    waitpid(pid, child_status, 0);
    return trace_id;
}

std::string AgentPart(const std::string& trace_id) {
    const auto separator = trace_id.find('^');
    return separator == std::string::npos ? std::string() :
                                            trace_id.substr(0, separator);
}

}  // namespace

int main() {
    it_test::ConfigureAgentEnvironment("cpp-it-fork", "cpp-it-fork");

    // CreateAgent is deliberately cold here. Each forked child calls Start()
    // independently and must receive a process-unique agent id.
    auto cold_agent = pinpoint::CreateAgent(
        pinpoint::APP_TYPE_CPP, "cpp-it-fork", {"--scenario=fork"}, {"fork"});

    int first_status = 0;
    int second_status = 0;
    const std::string first_trace = RunChild(cold_agent, 1, &first_status);
    const std::string second_trace = RunChild(cold_agent, 2, &second_status);
    const std::string first_agent = AgentPart(first_trace);
    const std::string second_agent = AgentPart(second_trace);
    const bool children_ok = WIFEXITED(first_status) && WEXITSTATUS(first_status) == 0 &&
                             WIFEXITED(second_status) && WEXITSTATUS(second_status) == 0;
    const bool unique = !first_agent.empty() && !second_agent.empty() &&
                        first_agent != second_agent;

    std::cout << "{\"status\":\"" << (children_ok && unique ? "ok" : "error")
              << "\",\"collector_host\":\""
              << it_test::JsonEscape(it_test::CollectorHost())
              << "\",\"first_agent_id\":\"" << it_test::JsonEscape(first_agent)
              << "\",\"second_agent_id\":\"" << it_test::JsonEscape(second_agent)
              << "\",\"unique_agent_ids\":" << it_test::JsonBool(unique) << "}"
              << std::endl;

    cold_agent->Shutdown();
    return children_ok && unique ? 0 : 1;
}
