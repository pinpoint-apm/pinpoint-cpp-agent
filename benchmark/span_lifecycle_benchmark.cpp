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

// What one instrumented request costs on the application thread: the whole
// NewSpan -> NewSpanEvent -> EndSpan lifecycle through the real AgentImpl,
// not one mechanism inside it.
//
// The other benchmarks here each isolate a single component (the id cache,
// the active-span registry, the runtime snapshot, the span queue) and answer
// the question "was this data structure worth it". None of them covers the
// admission path that stitches those components together, which is the
// number a host application actually pays and the one to quote as agent
// overhead. That gap is not theoretical: AgentImpl::tracing_active() carried
// a getpid() per span — ~1 ns on Darwin, ~150 ns on Linux/glibc, more than
// every component this directory measures put together — and no component
// benchmark could have shown it.
//
// Four request shapes, because they cost very different amounts and real
// traffic is a mix:
//   filtered   the URL is excluded, so NewSpan hands back the shared noop
//              span; the cheapest way to be instrumented and record nothing
//   unsampled  admitted but not sampled: an UnsampledSpan, still registered
//              in the active-request registry and still timed at EndSpan.
//              The majority path whenever sampling is on, so its cost is
//              what most requests in production actually pay
//   sampled    a full SpanImpl for a fresh root trace, with N span events
//   continued  a full SpanImpl for an inbound trace: the trace id is parsed
//              and every propagation header is copied into SpanData. Note
//              that a continued trace bypasses the sampler entirely (see
//              TraceSampler::isContinueSampled), which is why this shape is
//              always sampled regardless of the configured rate
//
// The gRPC clients are the production classes with their channels held
// unready, so the span worker collects batches and discards them the way it
// does during a collector outage. The queue therefore behaves as it does in
// production — chunks are dequeued and destroyed, not left to pile up into
// the head-drop path — without a network or a mock framework.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../src/agent.h"
#include "../src/config.h"
#include "../src/grpc.h"
#include "../include/pinpoint/tracer.h"

namespace pinpoint {
namespace benchmark {

    using Clock = std::chrono::steady_clock;

    // A handful of endpoints rather than one: a single operation name would
    // funnel every api-cache lookup into one shard and misrepresent the
    // sharding, while a unique name per request would miss the cache every
    // time. A small stable set is what a real service looks like.
    constexpr size_t kEndpointCount = 8;
    constexpr size_t kWarmupRequestsPerThread = 2000;
    constexpr size_t kLatencySampleStride = 16;  // time every Nth request
    constexpr std::string_view kFilteredUrl = "/bench/filtered";
    // Parsed by every "continued" request: agentId^startTime^sequence.
    constexpr std::string_view kInboundTraceId = "bench-agent^1700000000000^42";

    const char* const kEventOperations[] = {
        "mysql/query", "redis/get", "http/client",
    };
    constexpr size_t kEventOperationCount = std::size(kEventOperations);

    // Production gRPC clients with two overrides: create_stub() is neutered so
    // openChannel() cannot build a real stub, and readyChannel() reports false
    // so no worker ever dereferences one. Every send path checks readiness
    // first (run_span_worker, run_meta_worker, start_ping_stream), so this is
    // the collector-outage path, not an untested branch.
    class BenchGrpcAgent final : public GrpcAgent {
    public:
        explicit BenchGrpcAgent(std::shared_ptr<const Config> config)
            : GrpcAgent(std::move(config)) {}

        // Both the boot-phase registration and the periodic re-send go
        // through this virtual, so reporting success here is what brings the
        // agent online (enabled_ = true) with no collector present.
        GrpcRequestStatus registerAgent() override { return SEND_OK; }
        bool readyChannel() override { return false; }

    protected:
        void create_stub() override {}
    };

    class BenchGrpcMetadata final : public GrpcMetadata {
    public:
        explicit BenchGrpcMetadata(std::shared_ptr<const Config> config)
            : GrpcMetadata(std::move(config)) {}
        bool readyChannel() override { return false; }

    protected:
        void create_stub() override {}
    };

    class BenchGrpcSpan final : public GrpcSpan {
    public:
        explicit BenchGrpcSpan(std::shared_ptr<const Config> config)
            : GrpcSpan(std::move(config)) {}
        bool readyChannel() override { return false; }

    protected:
        void create_stub() override {}
    };

    class BenchGrpcStats final : public GrpcStats {
    public:
        explicit BenchGrpcStats(std::shared_ptr<const Config> config)
            : GrpcStats(std::move(config)) {}
        bool readyChannel() override { return false; }

    protected:
        void create_stub() override {}
    };

    // Serves the propagation headers an upstream Pinpoint agent would send.
    // Stateless, so one instance per thread is reused across requests — which
    // also matches how a host wraps its own request object once per call.
    class BenchContextReader final : public TraceContextReader {
    public:
        std::optional<std::string_view> Get(std::string_view key) const override {
            if (key == HEADER_TRACE_ID) return kInboundTraceId;
            if (key == HEADER_SPAN_ID) return "7446106466016340";
            if (key == HEADER_PARENT_SPAN_ID) return "1244562833205172";
            if (key == HEADER_PARENT_APP_NAME) return "upstream-service";
            if (key == HEADER_PARENT_APP_TYPE) return "1300";
            if (key == HEADER_FLAG) return "0";
            // Deliberately longer than BOTH small-string thresholds
            // (libstdc++ 15, libc++ 22): the propagation headers are copied
            // into SpanData, so a short host would keep every one of those
            // copies inside the string object and hide their cost on exactly
            // the platform this benchmark usually runs on. A real host name
            // is past both thresholds anyway.
            if (key == HEADER_HOST) return "upstream-gateway-07.internal:8443";
            return std::nullopt;
        }
    };

    struct Endpoint {
        std::string operation;
        std::string url;
    };

    std::vector<Endpoint> make_endpoints() {
        std::vector<Endpoint> endpoints;
        endpoints.reserve(kEndpointCount);
        for (size_t i = 0; i < kEndpointCount; i++) {
            const auto suffix = std::to_string(i);
            endpoints.push_back(Endpoint{"GET /api/v1/resource/" + suffix,
                                         "/api/v1/resource/" + suffix});
        }
        return endpoints;
    }

    // percent_rate picks the shape the sampler admits: 0 never samples a new
    // trace (UnsampledSpan), 100 always does (SpanImpl). Both clamp exactly in
    // PercentSampler, so neither phase depends on a counter landing right.
    std::shared_ptr<Config> make_bench_config(double percent_rate) {
        auto cfg = std::make_shared<Config>();
        cfg->enable = true;
        cfg->app_name_ = "bench-app";
        cfg->agent_id_ = "bench-agent";
        cfg->agent_name_ = "bench-agent-name";
        cfg->collector.host = "127.0.0.1";
        cfg->collector.agent_port = 9991;
        cfg->collector.stat_port = 9992;
        cfg->collector.span_port = 9993;
        cfg->sampling.type = "percent";
        cfg->sampling.percent_rate = percent_rate;
        // Leave sampling.new_throughput / cont_throughput at their defaults so
        // no rate limiter sits in front of the sampler and caps the loop.
        cfg->http.server.exclude_url = {std::string(kFilteredUrl)};
        // Stat and url-stat collection off: both are periodic worker-side
        // work, and url stats in particular would add a queue push per
        // request that the measured lifecycle does not otherwise perform.
        cfg->stat.enable = false;
        cfg->http.url_stat.enable = false;
        return cfg;
    }

    std::shared_ptr<AgentImpl> make_bench_agent(const std::shared_ptr<Config>& cfg) {
        return AgentImpl::createShared(cfg,
                                       std::make_unique<BenchGrpcAgent>(cfg),
                                       std::make_unique<BenchGrpcMetadata>(cfg),
                                       std::make_unique<BenchGrpcSpan>(cfg),
                                       std::make_unique<BenchGrpcStats>(cfg),
                                       nullptr,
                                       DEFAULT_APP_TYPE);
    }

    bool wait_enabled(const std::shared_ptr<AgentImpl>& agent,
                      std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            if (agent->Enable()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return agent->Enable();
    }

    enum class Shape { Filtered, Unsampled, Sampled, Continued };

    const char* shape_name(Shape shape) {
        switch (shape) {
        case Shape::Filtered:  return "filtered";
        case Shape::Unsampled: return "unsampled";
        case Shape::Sampled:   return "sampled";
        case Shape::Continued: return "continued";
        }
        return "?";
    }

    SpanPtr new_span(AgentImpl& agent, Shape shape, const Endpoint& endpoint,
                     TraceContextReader& reader) {
        switch (shape) {
        case Shape::Filtered:
            return agent.NewSpan(endpoint.operation, kFilteredUrl);
        case Shape::Continued:
            return agent.NewSpan(endpoint.operation, endpoint.url, reader);
        case Shape::Unsampled:
        case Shape::Sampled:
            break;
        }
        return agent.NewSpan(endpoint.operation, endpoint.url);
    }

    void run_request(AgentImpl& agent, Shape shape, const Endpoint& endpoint,
                     TraceContextReader& reader, size_t events) {
        auto span = new_span(agent, shape, endpoint, reader);
        for (size_t e = 0; e < events; e++) {
            auto* event = span->NewSpanEvent(kEventOperations[e % kEventOperationCount]);
            event->EndEvent();
        }
        span->EndSpan();
    }

    // One request of this shape must actually take the path the shape names.
    // Without this the whole phase can silently degrade to noop spans (an
    // agent that never came online, a filter that stopped matching, a sampler
    // rate that no longer clamps) and still report a very fast number.
    bool verify_shape(AgentImpl& agent, Shape shape, const Endpoint& endpoint) {
        BenchContextReader reader;
        auto span = new_span(agent, shape, endpoint, reader);
        const bool sampled = span->IsSampled();
        const bool has_span_id = span->GetSpanId() != 0;
        span->EndSpan();

        switch (shape) {
        // The noop span is the shared singleton: unsampled and span id 0.
        case Shape::Filtered:  return !sampled && !has_span_id;
        // UnsampledSpan reports no sampling but mints a real span id, which
        // is what separates it from the noop span above.
        case Shape::Unsampled: return !sampled && has_span_id;
        case Shape::Sampled:
        case Shape::Continued: return sampled && has_span_id;
        }
        return false;
    }

    struct PhaseResult {
        double nanoseconds_per_request{0.0};
        int64_t latency_p50{0};
        int64_t latency_p99{0};
        int64_t latency_max{0};
    };

    PhaseResult run_phase(AgentImpl& agent, Shape shape, const std::vector<Endpoint>& endpoints,
                          size_t thread_count, size_t requests_per_thread, size_t events) {
        std::atomic<size_t> ready{0};
        std::atomic<bool> go{false};
        std::vector<std::vector<int64_t>> sampled_latencies(thread_count);
        std::vector<std::thread> threads;
        threads.reserve(thread_count);

        for (size_t t = 0; t < thread_count; t++) {
            threads.emplace_back([&, t] {
                BenchContextReader reader;
                auto& samples = sampled_latencies[t];
                samples.reserve(requests_per_thread / kLatencySampleStride + 1);

                // Warm up on this thread specifically: the first request from
                // a thread pays one-time costs the steady state does not —
                // its shard id in the response-time and url-stat registries,
                // its thread-local noop-span owner and runtime snapshot, and
                // its home shard in the span queue.
                for (size_t i = 0; i < kWarmupRequestsPerThread; i++) {
                    run_request(agent, shape, endpoints[i % kEndpointCount], reader, events);
                }

                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                for (size_t i = 0; i < requests_per_thread; i++) {
                    const auto& endpoint = endpoints[i % kEndpointCount];
                    if (i % kLatencySampleStride == 0) {
                        const auto begin = Clock::now();
                        run_request(agent, shape, endpoint, reader, events);
                        samples.push_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin)
                                .count());
                    } else {
                        run_request(agent, shape, endpoint, reader, events);
                    }
                }
            });
        }

        while (ready.load(std::memory_order_acquire) < thread_count) {
            std::this_thread::yield();
        }
        const auto begin = Clock::now();
        go.store(true, std::memory_order_release);
        for (auto& thread : threads) {
            thread.join();
        }
        const auto wall_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();

        std::vector<int64_t> merged;
        for (auto& samples : sampled_latencies) {
            merged.insert(merged.end(), samples.begin(), samples.end());
        }
        std::sort(merged.begin(), merged.end());

        PhaseResult result;
        // Per THREAD, not per aggregate request: the question this benchmark
        // answers is what one request costs the application thread that makes
        // it, so a column that stays flat as threads are added means the agent
        // adds no contention, and one that grows is the cost to explain.
        // Dividing by the aggregate instead would fall with thread count and
        // read as "more threads, cheaper requests" — the opposite of what the
        // p99 beside it shows. Same convention as atomic_shared_ptr_benchmark.
        result.nanoseconds_per_request =
            static_cast<double>(wall_ns) / static_cast<double>(requests_per_thread);
        if (!merged.empty()) {
            // Nearest-rank percentiles, matching active_span_benchmark.
            const auto nearest_rank = [&merged](double fraction) {
                auto rank = static_cast<size_t>(
                    std::ceil(fraction * static_cast<double>(merged.size())));
                rank = std::min(std::max<size_t>(rank, 1), merged.size());
                return merged[rank - 1];
            };
            result.latency_p50 = nearest_rank(0.50);
            result.latency_p99 = nearest_rank(0.99);
            result.latency_max = merged.back();
        }
        return result;
    }

    void print_header() {
        std::cout << std::left
                  << std::setw(11) << "shape"
                  << std::right
                  << std::setw(8)  << "events"
                  << std::setw(9)  << "threads"
                  << std::setw(12) << "ns/req"
                  << std::setw(10) << "p50"
                  << std::setw(10) << "p99"
                  << std::setw(12) << "max" << "\n";
    }

    void print_row(Shape shape, size_t events, size_t thread_count, const PhaseResult& result) {
        std::cout << std::left
                  << std::setw(11) << shape_name(shape)
                  << std::right
                  << std::setw(8)  << events
                  << std::setw(9)  << thread_count
                  << std::setw(12) << std::fixed << std::setprecision(1)
                  << result.nanoseconds_per_request
                  << std::setw(10) << result.latency_p50
                  << std::setw(10) << result.latency_p99
                  << std::setw(12) << result.latency_max << "\n";
    }

    struct PhaseSpec {
        Shape shape;
        size_t events;
    };

    // Returns false when a shape did not take its intended path, so main()
    // can fail instead of publishing numbers for something else.
    bool run_agent_phases(const std::shared_ptr<AgentImpl>& agent,
                          const std::vector<PhaseSpec>& specs,
                          const std::vector<Endpoint>& endpoints,
                          size_t requests_per_thread) {
        static const size_t thread_counts[] = {1, 4, 8};
        for (const auto& spec : specs) {
            if (!verify_shape(*agent, spec.shape, endpoints[0])) {
                std::cerr << "shape verification failed: " << shape_name(spec.shape) << "\n";
                return false;
            }
            for (const auto thread_count : thread_counts) {
                // Every thread runs the same fixed load, so the total work
                // grows with the thread count and the reported per-request
                // cost stays comparable across the rows.
                const auto result = run_phase(*agent, spec.shape, endpoints, thread_count,
                                              requests_per_thread, spec.events);
                print_row(spec.shape, spec.events, thread_count, result);
            }
        }
        return true;
    }

}  // namespace benchmark
}  // namespace pinpoint

int main(int argc, char** argv) {
    using namespace pinpoint;
    using namespace pinpoint::benchmark;

    size_t requests_per_thread = 50000;
    if (argc > 1) {
        requests_per_thread = static_cast<size_t>(std::stoull(argv[1]));
    }

    const auto endpoints = make_endpoints();

    std::cout << "requests-per-thread=" << requests_per_thread
              << " endpoints=" << kEndpointCount
              << " (one request = NewSpan + N span events + EndSpan)\n";
    print_header();

    bool ok = true;

    // Sampling agent: admits every new trace, so it covers the three shapes
    // that do not depend on a sampler refusal.
    {
        auto cfg = make_bench_config(100.0);
        auto agent = make_bench_agent(cfg);
        agent->Start();
        if (!wait_enabled(agent)) {
            std::cerr << "sampling agent never came online\n";
            return 1;
        }
        ok = run_agent_phases(agent,
                              {{Shape::Filtered, 0},
                               {Shape::Sampled, 0},
                               {Shape::Sampled, 5},
                               {Shape::Continued, 5}},
                              endpoints, requests_per_thread);
        agent->Shutdown();
    }

    // Separate agent for the unsampled shape: the decision comes from the
    // configured rate, so it cannot share the one above. Run after it, not
    // alongside, so neither agent's worker threads perturb the other's numbers.
    if (ok) {
        auto cfg = make_bench_config(0.0);
        auto agent = make_bench_agent(cfg);
        agent->Start();
        if (!wait_enabled(agent)) {
            std::cerr << "unsampled agent never came online\n";
            return 1;
        }
        ok = run_agent_phases(agent, {{Shape::Unsampled, 0}}, endpoints, requests_per_thread);
        agent->Shutdown();
    }

    if (!ok) {
        return 1;
    }
    return 0;
}
