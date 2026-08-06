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

// Public-API benchmark used to compare one released agent against another.
//
// Unlike the microbenchmarks in the parent directory, which embed a legacy
// implementation next to the current one inside a single binary, this source is
// compiled twice — once per agent version, through pp_compat.h — and the two
// binaries are run against the same mock collector on the same machine.
//
// Every scenario drives the agent the way an instrumented application does, so
// what it reports is the cost an application actually pays, including the
// caller-side cost of whichever API shape that version exposes.
//
// A collector is mandatory: both versions only record real spans after
// AgentInfo registration succeeds, and hand out noop spans until then. Running
// without one measures nothing but the noop path.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>

#include "pp_compat.h"

// ---------------------------------------------------------------------------
// Per-thread allocation counting
// ---------------------------------------------------------------------------
// The agent runs worker threads throughout the measurement, so a global
// counter would attribute their allocations to the measured loop. A plain
// integral thread_local keeps the replacement allocation-free and counts only
// the thread under measurement.

namespace {
    thread_local uint64_t tls_allocations = 0;
}  // namespace

void* operator new(std::size_t size) {
    ++tls_allocations;
    // operator new must return a distinct non-null pointer for a zero-sized
    // request.
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

void* operator new[](std::size_t size) {
    ++tls_allocations;
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace pinpoint::benchmark {

    using Clock = std::chrono::steady_clock;

    constexpr size_t kLatencySampleEvery = 16;  // two clock reads would otherwise dominate
    // Only constants that exist in both versions are used, so the same source
    // compiles either way.
    constexpr int32_t kBenchServiceType = pinpoint::SERVICE_TYPE_CPP;
    constexpr int32_t kBenchMethodType = pinpoint::SERVICE_TYPE_CPP_FUNC;
    constexpr int32_t kBenchSqlType = pinpoint::SERVICE_TYPE_MYSQL_QUERY;

    struct ScenarioResult {
        std::string name;
        size_t threads = 1;
        size_t total_ops = 0;
        double ns_per_op = 0.0;
        double p50_ns = 0.0;
        double p99_ns = 0.0;
        double max_ns = 0.0;
        double allocations_per_op = 0.0;
        uint64_t checksum = 0;
        std::string note;
    };

    struct ThreadOutcome {
        std::vector<uint64_t> latency_samples;
        uint64_t allocations = 0;
        uint64_t checksum = 0;
    };

    // Nearest-rank percentile: the smallest sample with at least `fraction`
    // of all samples at or below it. Keep this definition in sync with
    // benchmark/active_span_benchmark.cpp so p99 means the same thing in
    // both reports (floor(fraction * (n-1)) biases one rank low at the
    // sample counts used here).
    double percentile(std::vector<uint64_t>& sorted_samples, double fraction) {
        if (sorted_samples.empty()) {
            return 0.0;
        }
        const auto n = sorted_samples.size();
        auto rank = static_cast<size_t>(
            std::ceil(fraction * static_cast<double>(n)));
        rank = std::min(std::max<size_t>(rank, 1), n);
        return static_cast<double>(sorted_samples[rank - 1]);
    }

    // Lazily registered metadata (api ids, string ids, sql ids) is the dominant
    // cold cost of the first few thousand spans, and it lands in whichever
    // scenario runs first. Warm each scenario long enough that the measured
    // loop sees only steady-state work.
    size_t g_warmup_ops = 5000;

    // Every scenario creates exactly one span per operation, so the number of
    // spans handed to the agent is a property of the schedule rather than
    // something the hot path has to count. Comparing this against what the
    // collector received is how a run proves it did not silently measure one
    // version's drop path against the other's send path.
    uint64_t g_recording_spans = 0;

    // Time given to the sender between scenarios so each one starts from a
    // drained queue instead of inheriting its predecessor's backlog.
    size_t g_drain_ms = 3000;

    // --- trace mode -------------------------------------------------------
    // Diagnosing tail latency needs the *time structure* of the spikes, which
    // percentile summaries erase: a spike every batch-send looks the same as
    // random scheduler noise in a p99 figure. In trace mode a single scenario
    // runs single-threaded with every operation timed and timestamped, and the
    // series is dumped for offline analysis.
    std::string g_only_scenario;   // when set, all other scenarios are skipped
    std::string g_trace_path;      // when set, dump per-op (t, latency) series

    struct TraceSample {
        uint64_t offset_ns;   // since scenario start
        uint64_t latency_ns;
        // Phase decomposition (filled for scenarios that support it): where
        // inside the operation the time went. A latency spike whose excess sits
        // in one phase names the mechanism; one smeared across all phases
        // points at process-wide interference (allocator, scheduler).
        uint64_t phase_new_span_ns = 0;
        uint64_t phase_events_ns = 0;
        uint64_t phase_end_span_ns = 0;
        uint64_t allocations = 0;
    };
    std::vector<TraceSample> g_trace;

    // Set while tracing a scenario that reports per-phase timings.
    bool g_phase_trace = false;
    thread_local uint64_t tls_phase_new_span = 0;
    thread_local uint64_t tls_phase_events = 0;
    thread_local uint64_t tls_phase_end_span = 0;

    // Runs `body` on `threads` threads, `ops_per_thread` times each, with all
    // threads released from a common start gate so contention is real. Pass
    // records=false for a scenario whose spans never reach the collector.
    template<typename Body>
    ScenarioResult run_scenario(const std::string& name, size_t threads,
                                size_t ops_per_thread, Body&& body,
                                bool records = true) {
        if (!g_only_scenario.empty() && name != g_only_scenario) {
            return {};  // empty name; the caller's emit() skips it
        }
        const bool tracing = !g_trace_path.empty() && threads == 1;
        std::atomic<size_t> ready{0};
        std::atomic<bool> go{false};
        std::vector<ThreadOutcome> outcomes(threads);
        std::vector<std::thread> workers;
        workers.reserve(threads);

        for (size_t t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                auto& outcome = outcomes[t];
                outcome.latency_samples.reserve(ops_per_thread / kLatencySampleEvery + 1);

                // Warm the thread's caches and any lazily created thread-local
                // state so the measured loop sees a steady state.
                for (size_t i = 0; i < g_warmup_ops; ++i) {
                    outcome.checksum += body(t, i);
                }

                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                const uint64_t allocations_before = tls_allocations;
                if (tracing) {
                    // Every op is timed and timestamped. The two clock reads
                    // per op inflate the absolute numbers slightly, but the
                    // spike *pattern* is what trace mode is for.
                    g_trace.clear();
                    g_trace.reserve(ops_per_thread);
                    const auto trace_start = Clock::now();
                    for (size_t i = 0; i < ops_per_thread; ++i) {
                        const uint64_t allocs_before = tls_allocations;
                        const auto started = Clock::now();
                        outcome.checksum += body(t, i);
                        const auto ended = Clock::now();
                        const auto latency = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count());
                        TraceSample sample;
                        sample.offset_ns = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(started - trace_start).count());
                        sample.latency_ns = latency;
                        sample.phase_new_span_ns = tls_phase_new_span;
                        sample.phase_events_ns = tls_phase_events;
                        sample.phase_end_span_ns = tls_phase_end_span;
                        sample.allocations = tls_allocations - allocs_before;
                        g_trace.push_back(sample);
                        outcome.latency_samples.push_back(latency);
                    }
                } else {
                    for (size_t i = 0; i < ops_per_thread; ++i) {
                        if (i % kLatencySampleEvery == 0) {
                            const auto started = Clock::now();
                            outcome.checksum += body(t, i);
                            const auto elapsed = Clock::now() - started;
                            outcome.latency_samples.push_back(
                                static_cast<uint64_t>(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
                        } else {
                            outcome.checksum += body(t, i);
                        }
                    }
                }
                outcome.allocations = tls_allocations - allocations_before;
            });
        }

        while (ready.load(std::memory_order_acquire) < threads) {
            std::this_thread::yield();
        }
        const auto started = Clock::now();
        go.store(true, std::memory_order_release);
        for (auto& worker : workers) {
            worker.join();
        }
        const auto elapsed = Clock::now() - started;

        ScenarioResult result;
        result.name = name;
        result.threads = threads;
        result.total_ops = ops_per_thread * threads;

        const auto elapsed_ns =
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        // Wall time divided by per-thread ops: an unchanged value as threads
        // rise means the path is not accumulating cross-core contention.
        result.ns_per_op = elapsed_ns / static_cast<double>(ops_per_thread);

        std::vector<uint64_t> samples;
        uint64_t allocations = 0;
        for (auto& outcome : outcomes) {
            samples.insert(samples.end(), outcome.latency_samples.begin(), outcome.latency_samples.end());
            allocations += outcome.allocations;
            result.checksum += outcome.checksum;
        }
        std::sort(samples.begin(), samples.end());
        result.p50_ns = percentile(samples, 0.50);
        result.p99_ns = percentile(samples, 0.99);
        result.max_ns = samples.empty() ? 0.0 : static_cast<double>(samples.back());
        result.allocations_per_op =
            static_cast<double>(allocations) / static_cast<double>(result.total_ops);

        if (records) {
            g_recording_spans += static_cast<uint64_t>((ops_per_thread + g_warmup_ops) * threads);
        }

        // Let the sender catch up before the next scenario is timed.
        std::this_thread::sleep_for(std::chrono::milliseconds(g_drain_ms));
        return result;
    }

    // ---- shared fixtures --------------------------------------------------

    std::vector<std::pair<std::string, std::string>> inbound_headers() {
        return {
            {"Host", "bench.local:8080"},
            {"User-Agent", "pinpoint-version-compare/1.0"},
            {"Accept", "*/*"},
            {"Content-Type", "application/json"},
            {"X-Forwarded-For", "10.0.0.7"},
        };
    }

    // Carries an explicit unsampled decision so the agent takes the
    // continue-unsampled path without changing process-wide sampling config.
    std::vector<std::pair<std::string, std::string>> unsampled_headers() {
        auto headers = inbound_headers();
        headers.emplace_back(std::string(pinpoint::HEADER_SAMPLED), "s0");
        headers.emplace_back(std::string(pinpoint::HEADER_TRACE_ID), "bench-agent^1700000000000^42");
        headers.emplace_back(std::string(pinpoint::HEADER_SPAN_ID), "7777");
        headers.emplace_back(std::string(pinpoint::HEADER_PARENT_SPAN_ID), "8888");
        headers.emplace_back(std::string(pinpoint::HEADER_FLAG), "0");
        return headers;
    }

    std::vector<std::string> sql_statement_pool(size_t count) {
        std::vector<std::string> statements;
        statements.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            statements.push_back(
                "SELECT o.id, o.total, c.name FROM orders_" + std::to_string(i) +
                " o JOIN customers c ON c.id = o.customer_id "
                "WHERE o.status = 'SHIPPED' AND o.created_at > ? AND o.total > ? "
                "ORDER BY o.created_at DESC LIMIT 100");
        }
        return statements;
    }

    // ---- scenarios --------------------------------------------------------

    // S1: the shape of a traced inbound request — one sampled span with a few
    // child events. Exercises the metadata id caches, the per-span allocation
    // path, and the span queue enqueue at EndSpan.
    uint64_t span_lifecycle(const ppc::AgentPtr& agent, ppc::VectorHeaderReader& reader) {
        // The phase clock reads only run in trace mode; a normal run takes the
        // straight-line path with a single branch per phase boundary.
        const bool phases = g_phase_trace;
        const auto t0 = phases ? Clock::now() : Clock::time_point{};

        auto span = agent->NewSpan("BenchController.handle()", "/bench/order", "GET", reader);
        span->SetServiceType(kBenchServiceType);
        span->SetRemoteAddress("10.0.0.7");
        span->SetEndPoint("bench.local:8080");

        const auto t1 = phases ? Clock::now() : Clock::time_point{};

        for (int i = 0; i < 3; ++i) {
            auto event = span->NewSpanEvent("OrderService.step()", kBenchMethodType);
            event->SetEndPoint("bench.local:8080");
            ppc::EndEvent(span, event);
        }

        const auto t2 = phases ? Clock::now() : Clock::time_point{};

        span->SetStatusCode(200);
        const auto span_id = span->GetSpanId();
        span->EndSpan();

        if (phases) {
            const auto t3 = Clock::now();
            const auto ns = [](Clock::duration d) {
                return static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
            };
            tls_phase_new_span = ns(t1 - t0);
            tls_phase_events = ns(t2 - t1);
            tls_phase_end_span = ns(t3 - t2);
        }
        return static_cast<uint64_t>(span_id);
    }

    // S3: annotation-heavy span. v1.1.0 hands out a shared_ptr<Annotation> per
    // call site and stores through the abstract container; main records
    // directly on the span/event.
    uint64_t annotation_heavy(const ppc::AgentPtr& agent, ppc::VectorHeaderReader& reader) {
        auto span = agent->NewSpan("BenchController.annotated()", "/bench/annotated", "POST", reader);
        span->SetServiceType(kBenchServiceType);

        ppc::AnnotateString(span, pinpoint::ANNOTATION_HTTP_URL, "/bench/annotated?page=3");
        ppc::AnnotateInt(span, pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);

        auto event = span->NewSpanEvent("OrderService.annotate()", kBenchMethodType);
        ppc::AnnotateString(event, pinpoint::ANNOTATION_API, "OrderService.annotate(String, int)");
        ppc::AnnotateInt(event, pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);
        ppc::AnnotateLong(event, pinpoint::ANNOTATION_API, 1700000000000LL);
        ppc::AnnotateString(event, pinpoint::ANNOTATION_HTTP_URL, "https://downstream.local/api/v2/orders");
        ppc::AnnotateStringString(event, pinpoint::ANNOTATION_HTTP_REQUEST_HEADER, "page", "3");
        ppc::AnnotateStringString(event, pinpoint::ANNOTATION_HTTP_REQUEST_HEADER, "sort", "created_at:desc");
        ppc::EndEvent(span, event);

        const auto span_id = span->GetSpanId();
        span->EndSpan();
        return static_cast<uint64_t>(span_id);
    }

    // S4: SQL recording. The repeated-statement case is the raw SQL cache hit
    // path; the rotating-pool case is larger than the cache so every lookup
    // misses and pays normalization plus insert and eviction churn.
    uint64_t sql_span(const ppc::AgentPtr& agent, ppc::VectorHeaderReader& reader,
                      const std::string& statement, const std::vector<std::string>& binds) {
        auto span = agent->NewSpan("BenchController.query()", "/bench/query", "GET", reader);
        span->SetServiceType(kBenchServiceType);

        auto event = span->NewSpanEvent("OrderRepository.find()", kBenchSqlType);
        event->SetDestination("orders-db");
        event->SetEndPoint("mysql.local:3306");
        ppc::SetSqlQuery(event, statement, binds);
        ppc::EndEvent(span, event);

        const auto span_id = span->GetSpanId();
        span->EndSpan();
        return static_cast<uint64_t>(span_id);
    }

    // S5: event-shape stress. `depth` nests events before unwinding them;
    // `width` records them one after another.
    uint64_t nested_events(const ppc::AgentPtr& agent, ppc::VectorHeaderReader& reader,
                           size_t depth, size_t width) {
        auto span = agent->NewSpan("BenchController.nested()", "/bench/nested", "GET", reader);
        span->SetServiceType(kBenchServiceType);

        std::vector<ppc::EventPtr> stack;
        stack.reserve(depth);
        for (size_t i = 0; i < depth; ++i) {
            stack.push_back(span->NewSpanEvent("Nested.level()", kBenchMethodType));
        }
        for (size_t i = depth; i > 0; --i) {
            ppc::EndEvent(span, stack[i - 1]);
        }
        for (size_t i = 0; i < width; ++i) {
            auto event = span->NewSpanEvent("Sequential.step()", kBenchMethodType);
            ppc::EndEvent(span, event);
        }

        const auto span_id = span->GetSpanId();
        span->EndSpan();
        return static_cast<uint64_t>(span_id);
    }

    // S7: outbound context propagation. v1.1.0 injects from the span, main from
    // the span event that represents the outbound call.
    uint64_t propagation(const ppc::AgentPtr& agent, ppc::VectorHeaderReader& reader,
                         ppc::VectorContextWriter& writer) {
        auto span = agent->NewSpan("BenchController.call()", "/bench/call", "GET", reader);
        span->SetServiceType(kBenchServiceType);

        auto event = span->NewSpanEvent("HttpClient.get()", pinpoint::SERVICE_TYPE_CPP_HTTP_CLIENT);
        event->SetDestination("downstream.local");
        event->SetEndPoint("downstream.local:8081");
        writer.Clear();
        ppc::InjectContext(span, event, writer);
        ppc::EndEvent(span, event);

        const auto injected = writer.size();
        span->EndSpan();
        return static_cast<uint64_t>(injected);
    }

    // ---- reporting --------------------------------------------------------

    void emit(const ScenarioResult& result) {
        if (result.name.empty()) {
            return;  // scenario was filtered out by --scenario
        }
        // Machine-readable line consumed by run_compare.sh; the human table is
        // printed separately at the end.
        std::cout << "RESULT\t" << ppc::kApiVariant
                  << '\t' << result.name
                  << '\t' << result.threads
                  << '\t' << std::fixed << std::setprecision(1) << result.ns_per_op
                  << '\t' << result.p50_ns
                  << '\t' << result.p99_ns
                  << '\t' << result.max_ns
                  << '\t' << std::setprecision(3) << result.allocations_per_op
                  << '\t' << result.total_ops
                  << '\t' << (result.note.empty() ? "-" : result.note)
                  << std::endl;
    }

    long peak_rss_kib() {
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) != 0) {
            return 0;
        }
        // macOS reports bytes, Linux reports kibibytes.
#if defined(__APPLE__)
        return usage.ru_maxrss / 1024;
#else
        return usage.ru_maxrss;
#endif
    }

    std::string build_config(const std::string& host, int agent_port, int span_port, int stat_port,
                             int batch_size, int flush_interval_ms) {
        // Uses the Collector.Grpc* key spelling: it is what v1.1.0 reads, and
        // the current agent still honors it as a documented deprecated alias,
        // so one config string drives both versions identically.
        std::string batch_overrides;
        if (batch_size > 0 || flush_interval_ms > 0) {
            // Diagnosis knobs; only the current agent reads SpanBatch keys,
            // v1.1.0 ignores them.
            batch_overrides += "  SpanBatch:\n";
            if (batch_size > 0) {
                batch_overrides += "    Size: " + std::to_string(batch_size) + "\n";
            }
            if (flush_interval_ms > 0) {
                batch_overrides += "    FlushIntervalMs: " + std::to_string(flush_interval_ms) + "\n";
            }
        }
        return
            "ApplicationName: cpp-version-compare\n"
            "Collector:\n"
            "  GrpcHost: " + host + "\n"
            "  GrpcAgentPort: " + std::to_string(agent_port) + "\n"
            "  GrpcSpanPort: " + std::to_string(span_port) + "\n"
            "  GrpcStatPort: " + std::to_string(stat_port) + "\n"
            + batch_overrides +
            "Sampling:\n"
            "  Type: COUNTER\n"
            "  CounterRate: 1\n"
            // Sized to absorb a whole scenario's spans without dropping, which
            // the shipped default of 1024 does not. That default makes the two
            // versions do *unequal* work: whichever version's sender drains
            // more slowly saturates first and then takes a cheap drop path
            // instead of serializing and sending, so it measures as faster
            // while actually delivering less. Comparing per-span cost requires
            // both versions to deliver every span. The cost is that peak RSS
            // now reflects this queue size rather than a shipped default.
            //
            // 65536 is MAX_SPAN_QUEUE_SIZE in both versions. Anything larger is
            // rejected and silently reset to the 1024 default, which is far too
            // small to absorb a scenario's generation burst.
            "Span:\n"
            "  QueueSize: 65536\n"
            "Stat:\n"
            "  Enable: false\n"
            "Log:\n"
            "  Level: error\n";
    }

    struct Options {
        std::string host = "127.0.0.1";
        int agent_port = 9991;
        int span_port = 9993;
        int stat_port = 9992;
        // Kept modest on purpose: one scenario's spans must fit in the queue so
        // no version drops, and a longer scenario only buys precision that the
        // machine's own run-to-run spread swamps anyway.
        size_t ops = 2500;
        size_t warmup = 1000;
        size_t drain_ms = 3000;
        std::string only_scenario;
        std::string trace_path;
        int batch_size = 0;          // 0 = leave the version's default
        int flush_interval_ms = 0;   // 0 = leave the version's default
        // Capped below the core count on purpose: the agent runs its own gRPC
        // worker threads in this process, so higher counts measure scheduler
        // oversubscription rather than the tracing path.
        std::vector<size_t> thread_counts{1, 2, 4, 8};
    };

    Options parse_options(int argc, char** argv) {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto next = [&]() -> std::string {
                return (i + 1 < argc) ? argv[++i] : std::string{};
            };
            if (arg == "--host") {
                options.host = next();
            } else if (arg == "--agent-port") {
                options.agent_port = std::stoi(next());
            } else if (arg == "--span-port") {
                options.span_port = std::stoi(next());
            } else if (arg == "--stat-port") {
                options.stat_port = std::stoi(next());
            } else if (arg == "--ops") {
                options.ops = static_cast<size_t>(std::stoul(next()));
            } else if (arg == "--warmup") {
                options.warmup = static_cast<size_t>(std::stoul(next()));
            } else if (arg == "--drain-ms") {
                options.drain_ms = static_cast<size_t>(std::stoul(next()));
            } else if (arg == "--scenario") {
                options.only_scenario = next();
            } else if (arg == "--trace") {
                options.trace_path = next();
            } else if (arg == "--batch-size") {
                options.batch_size = std::stoi(next());
            } else if (arg == "--flush-interval-ms") {
                options.flush_interval_ms = std::stoi(next());
            }
        }
        return options;
    }

    int run(int argc, char** argv) {
        const auto options = parse_options(argc, argv);
        g_warmup_ops = options.warmup;
        g_drain_ms = options.drain_ms;
        g_only_scenario = options.only_scenario;
        g_trace_path = options.trace_path;
        // Phase decomposition is implemented for the s1 workload (also the one
        // s6 reuses); other scenarios trace total latency only.
        g_phase_trace = !options.trace_path.empty() &&
                        options.only_scenario == "s1_span_lifecycle";
        if (!options.trace_path.empty() && options.only_scenario.empty()) {
            // Every single-threaded scenario overwrites g_trace, so without
            // --scenario the dump silently holds whichever ran last — a
            // plausible-looking file for the wrong workload.
            std::cerr << "[" << ppc::kApiVariant << "] warning: --trace without "
                         "--scenario records only the LAST single-threaded "
                         "scenario; pass --scenario <name> to pick the workload "
                         "(phase columns need --scenario s1_span_lifecycle)"
                      << std::endl;
        }

        std::cerr << "[" << ppc::kApiVariant << "] starting agent against "
                  << options.host << " agent=" << options.agent_port
                  << " span=" << options.span_port << " stat=" << options.stat_port << std::endl;

        auto agent = ppc::StartAgent(
            build_config(options.host, options.agent_port, options.span_port, options.stat_port,
                         options.batch_size, options.flush_interval_ms));
        if (!ppc::WaitUntilEnabled(agent, std::chrono::seconds(30))) {
            std::cerr << "[" << ppc::kApiVariant
                      << "] agent never reported Enable()==true; a collector must be reachable, "
                         "otherwise only noop spans are measured" << std::endl;
            return 1;
        }
        std::cerr << "[" << ppc::kApiVariant << "] agent enabled" << std::endl;

        auto headers = inbound_headers();
        auto unsampled = unsampled_headers();
        const auto statements = sql_statement_pool(4096);
        const std::vector<std::string> binds{"1700000000000", "99.50", "SHIPPED", "100"};
        const std::vector<std::string> no_binds{};

        std::vector<ScenarioResult> results;

        // Verify the sampled/unsampled split actually happened; a silent
        // fallback to noop spans would make every number meaningless.
        {
            ppc::VectorHeaderReader reader(headers);
            auto sampled_span = agent->NewSpan("probe", "/probe", "GET", reader);
            const bool sampled = sampled_span->IsSampled();
            sampled_span->EndSpan();

            ppc::VectorHeaderReader unsampled_reader(unsampled);
            auto unsampled_span = agent->NewSpan("probe", "/probe", "GET", unsampled_reader);
            const bool unsampled_ok = !unsampled_span->IsSampled();
            unsampled_span->EndSpan();

            std::cerr << "[" << ppc::kApiVariant << "] probe: sampled=" << sampled
                      << " unsampled_path=" << unsampled_ok << std::endl;
            if (!sampled) {
                std::cerr << "[" << ppc::kApiVariant
                          << "] sampled probe span was not sampled; aborting" << std::endl;
                return 1;
            }
            if (!unsampled_ok) {
                std::cerr << "[" << ppc::kApiVariant
                          << "] WARNING: Pinpoint-Sampled:s0 did not yield an unsampled span; "
                             "s2_unsampled measures the sampled path on this version" << std::endl;
            }
        }

        results.push_back(run_scenario("s1_span_lifecycle", 1, options.ops,
            [&](size_t, size_t) {
                thread_local ppc::VectorHeaderReader reader(inbound_headers());
                return span_lifecycle(agent, reader);
            }));

        results.push_back(run_scenario("s2_unsampled", 1, options.ops,
            [&](size_t, size_t) {
                thread_local ppc::VectorHeaderReader reader(unsampled_headers());
                auto span = agent->NewSpan("BenchController.handle()", "/bench/order", "GET", reader);
                auto event = span->NewSpanEvent("OrderService.step()", kBenchMethodType);
                ppc::AnnotateString(event, pinpoint::ANNOTATION_HTTP_URL, "/bench/order");
                ppc::EndEvent(span, event);
                span->EndSpan();
                return uint64_t{1};
            },
            // Unsampled spans are never queued, so they must not count toward
            // the spans the collector is expected to receive.
            false));

        results.push_back(run_scenario("s3_annotation_heavy", 1, options.ops,
            [&](size_t, size_t) {
                thread_local ppc::VectorHeaderReader reader(inbound_headers());
                return annotation_heavy(agent, reader);
            }));

        results.push_back(run_scenario("s4a_sql_hit", 1, options.ops,
            [&](size_t, size_t) {
                thread_local ppc::VectorHeaderReader reader(inbound_headers());
                return sql_span(agent, reader, statements[0], no_binds);
            }));

        results.push_back(run_scenario("s4b_sql_hit_binds", 1, options.ops,
            [&](size_t, size_t) {
                thread_local ppc::VectorHeaderReader reader(inbound_headers());
                return sql_span(agent, reader, statements[0], binds);
            }));

        results.push_back(run_scenario("s4c_sql_miss", 1, options.ops,
            [&](size_t, size_t index) {
                thread_local ppc::VectorHeaderReader reader(inbound_headers());
                return sql_span(agent, reader, statements[index % statements.size()], no_binds);
            }));

        results.push_back(run_scenario("s5a_deep_events", 1, options.ops / 4,
            [&](size_t, size_t) {
                thread_local ppc::VectorHeaderReader reader(inbound_headers());
                return nested_events(agent, reader, 30, 0);
            }));

        results.push_back(run_scenario("s5b_wide_events", 1, options.ops / 4,
            [&](size_t, size_t) {
                thread_local ppc::VectorHeaderReader reader(inbound_headers());
                return nested_events(agent, reader, 0, 100);
            }));

        results.push_back(run_scenario("s7_propagation", 1, options.ops,
            [&](size_t, size_t) {
                thread_local ppc::VectorHeaderReader reader(inbound_headers());
                thread_local ppc::VectorContextWriter writer;
                return propagation(agent, reader, writer);
            }));

        // S6: the same request shape as S1 under rising thread counts. This is
        // where shared hot spots (span queue lock, metadata cache rwlock,
        // runtime snapshot refcount) show up as ns/op growth.
        for (const auto threads : options.thread_counts) {
            results.push_back(run_scenario("s6_threads_" + std::to_string(threads), threads, options.ops,
                [&](size_t, size_t) {
                    thread_local ppc::VectorHeaderReader reader(inbound_headers());
                    return span_lifecycle(agent, reader);
                }));
        }

        for (const auto& result : results) {
            emit(result);
        }
        std::cout << "PEAKRSS\t" << ppc::kApiVariant << '\t' << peak_rss_kib() << std::endl;
        // The probe spans above are recorded too.
        std::cout << "SPANS\t" << ppc::kApiVariant << '\t' << (g_recording_spans + 1) << std::endl;

        if (!g_trace_path.empty() && !g_trace.empty()) {
            if (FILE* out = std::fopen(g_trace_path.c_str(), "w")) {
                std::fprintf(out, "offset_ns\tlatency_ns\tnew_span_ns\tevents_ns\tend_span_ns\tallocs\n");
                for (const auto& sample : g_trace) {
                    std::fprintf(out, "%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n",
                                 static_cast<unsigned long long>(sample.offset_ns),
                                 static_cast<unsigned long long>(sample.latency_ns),
                                 static_cast<unsigned long long>(sample.phase_new_span_ns),
                                 static_cast<unsigned long long>(sample.phase_events_ns),
                                 static_cast<unsigned long long>(sample.phase_end_span_ns),
                                 static_cast<unsigned long long>(sample.allocations));
                }
                std::fclose(out);
                std::cerr << "[" << ppc::kApiVariant << "] trace: " << g_trace.size()
                          << " ops -> " << g_trace_path << std::endl;
            } else {
                std::cerr << "[" << ppc::kApiVariant << "] failed to open trace file "
                          << g_trace_path << std::endl;
            }
        }

        agent->Shutdown();
        return 0;
    }

}  // namespace pinpoint::benchmark

int main(int argc, char** argv) {
    return pinpoint::benchmark::run(argc, argv);
}
