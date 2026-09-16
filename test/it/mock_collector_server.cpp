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

/**
 * Standalone wrapper around the integration suite's MockCollector.
 *
 * The gtest suite drives the same collector in-process, which cannot express
 * one thing a soak needs: a collector that dies with its process and comes
 * back on the same ports. Running it as its own process makes SIGKILL the
 * fault-injection API, so the agent sees a real connection refusal from a
 * vanished host rather than a cooperative endpoint shutdown.
 *
 * Usage:
 *   mock_collector_server [--agent-port N] [--span-port N] [--stat-port N]
 *                         [--report-interval SEC]
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <unistd.h>

#include "mock_collector.h"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

[[noreturn]] void usage(const char* argv0, int code) {
    std::fprintf(code == 0 ? stdout : stderr,
                 "Usage: %s [--agent-port N] [--span-port N] [--stat-port N]\n"
                 "          [--report-interval SEC]\n"
                 "Defaults: 9991/9993/9992, report every 30 s.\n",
                 argv0);
    std::exit(code);
}

int parse_port(const char* argv0, const char* value) {
    char* end = nullptr;
    const long port = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || port <= 0 || port > 65535) {
        std::fprintf(stderr, "invalid port: %s\n", value);
        usage(argv0, 2);
    }
    return static_cast<int>(port);
}

}  // namespace

int main(int argc, char** argv) {
    int agent_port = 9991;
    int span_port = 9993;
    int stat_port = 9992;
    int report_interval = 30;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if ((arg == "--agent-port" || arg == "--span-port" ||
             arg == "--stat-port" || arg == "--report-interval") && !has_value) {
            std::fprintf(stderr, "%s requires a value\n", arg.c_str());
            usage(argv[0], 2);
        }
        if (arg == "--agent-port") {
            agent_port = parse_port(argv[0], argv[++i]);
        } else if (arg == "--span-port") {
            span_port = parse_port(argv[0], argv[++i]);
        } else if (arg == "--stat-port") {
            stat_port = parse_port(argv[0], argv[++i]);
        } else if (arg == "--report-interval") {
            report_interval = std::atoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0], 0);
        } else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage(argv[0], 2);
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    // A collector that vanishes leaves half-open streams behind; writing to one
    // must not take this process down when it is the one being restarted.
    std::signal(SIGPIPE, SIG_IGN);

    pinpoint::test::MockCollector collector(agent_port, span_port, stat_port);
    if (!collector.Start()) {
        std::fprintf(stderr,
                     "failed to bind mock collector on %d/%d/%d "
                     "(ports already in use?)\n",
                     agent_port, span_port, stat_port);
        return 1;
    }

    std::printf("mock collector ready: agent=%d span=%d stat=%d pid=%d\n",
                collector.agent_port(), collector.span_port(),
                collector.stat_port(), static_cast<int>(getpid()));
    std::fflush(stdout);

    const auto started = std::chrono::steady_clock::now();
    auto next_report = started + std::chrono::seconds(report_interval);
    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (report_interval <= 0 ||
            std::chrono::steady_clock::now() < next_report) {
            continue;
        }
        next_report += std::chrono::seconds(report_interval);

        // The counts are the collector-side proof that the agent reconnected
        // after a kill: they restart from zero with the process, so a report
        // that stays at zero means this incarnation was never reached.
        const auto snapshot = collector.snapshot();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        std::printf(
            "[+%llds] agent_infos=%zu pings=%zu span_batches=%zu spans=%zu "
            "stats=%zu api_meta=%zu sql_meta=%zu string_meta=%zu rpcs=%zu\n",
            static_cast<long long>(elapsed), snapshot.agent_infos.size(),
            snapshot.pings.size(), snapshot.span_batches.size(),
            snapshot.span_messages.size(), snapshot.stats.size(),
            snapshot.api_metadata.size(), snapshot.sql_metadata.size(),
            snapshot.string_metadata.size(), snapshot.rpc_results.size());
        std::fflush(stdout);
    }

    std::printf("mock collector stopping\n");
    std::fflush(stdout);
    collector.Shutdown();
    return 0;
}
