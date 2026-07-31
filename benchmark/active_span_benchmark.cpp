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

// Compares the former active-span registry (sharded unordered_map keyed by
// span id — one map-node malloc/free per request) with the intrusive-list
// ActiveSpanRegistry that replaced it. Every request registers at span start
// and deregisters at EndSpan, so the measured unit is one add+drop pair.
// The new implementation is included from src/active_span.h, so the
// benchmark measures the exact production code.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "active_span.h"

namespace pinpoint {
namespace benchmark {

    using Clock = std::chrono::steady_clock;

    constexpr size_t kShardCount = 64;          // matches ActiveSpanRegistry
    constexpr size_t kHandleRingSize = 64;      // distinct node addresses per thread
    constexpr size_t kResidentSpans = 256;      // steady in-flight population
    constexpr size_t kWarmupPairsPerThread = 10000;
    constexpr size_t kLatencySampleStride = 64; // time every Nth pair
    constexpr int64_t kBaseStartTime = 1700000000000; // fixed epoch-ms base

    // The registry AgentStats used before the intrusive-node change, copied
    // verbatim (shard layout, alignment, try_emplace insert) so the
    // comparison is against the replaced implementation, not a strawman.
    class LegacyMapRegistry final {
    public:
        struct Handle {};  // the map needs no per-span state in the caller

        void add(Handle&, int64_t span_id, int64_t start_time) {
            auto& shard = shardOf(span_id);
            std::lock_guard<std::mutex> lock(shard.mutex_);
            shard.spans_.try_emplace(span_id, start_time);
        }

        void drop(Handle&, int64_t span_id) {
            auto& shard = shardOf(span_id);
            std::lock_guard<std::mutex> lock(shard.mutex_);
            shard.spans_.erase(span_id);
        }

        void collect(int32_t buckets[4], int64_t sample_time_ms) {
            buckets[0] = 0;
            buckets[1] = 0;
            buckets[2] = 0;
            buckets[3] = 0;
            for (auto& shard : shards_) {
                std::lock_guard<std::mutex> lock(shard.mutex_);
                for (const auto& iter : shard.spans_) {
                    auto active_time = sample_time_ms - iter.second;
                    if (active_time < 1000) {
                        buckets[0]++;
                    } else if (active_time < 3000) {
                        buckets[1]++;
                    } else if (active_time < 5000) {
                        buckets[2]++;
                    } else {
                        buckets[3]++;
                    }
                }
            }
        }

    private:
        struct alignas(64) Shard {
            std::mutex mutex_;
            std::unordered_map<int64_t, int64_t> spans_;
        };

        Shard& shardOf(int64_t span_id) {
            return shards_[std::hash<int64_t>{}(span_id) % shards_.size()];
        }

        std::array<Shard, kShardCount> shards_;
    };

    // Adapter giving the production registry the same driver-facing shape as
    // the legacy one (drop takes the unused span id).
    class IntrusiveRegistry final {
    public:
        using Handle = ActiveSpanNode;

        void add(Handle& handle, int64_t span_id, int64_t start_time) {
            registry_.add(handle, span_id, start_time);
        }

        void drop(Handle& handle, int64_t) { registry_.drop(handle); }

        void collect(int32_t buckets[4], int64_t sample_time_ms) {
            registry_.collect(buckets, sample_time_ms);
        }

    private:
        ActiveSpanRegistry registry_;
    };

    // Deterministic 64-bit mix emulating the agent's random span ids; both
    // registries see the identical per-thread id stream.
    uint64_t splitmix64(uint64_t& state) {
        uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    struct PhaseResult {
        double nanoseconds_per_pair{0.0};
        int64_t latency_p50{0};
        int64_t latency_p99{0};
        int64_t latency_max{0};
    };

    template <typename Registry>
    PhaseResult run_phase(Registry& registry, size_t thread_count, size_t pairs_per_thread) {
        std::atomic<size_t> ready{0};
        std::atomic<bool> go{false};
        std::vector<std::vector<int64_t>> sampled_latencies(thread_count);
        std::vector<std::thread> threads;
        threads.reserve(thread_count);

        for (size_t t = 0; t < thread_count; t++) {
            threads.emplace_back([&, t] {
                // Ring of handles so the intrusive nodes span multiple cache
                // lines the way nodes embedded in distinct span objects do,
                // instead of hammering one hot stack slot.
                std::vector<typename Registry::Handle> ring(kHandleRingSize);
                auto& samples = sampled_latencies[t];
                samples.reserve(pairs_per_thread / kLatencySampleStride + 1);
                uint64_t rng_state = 0x1234567890abcdefull ^ (t * 0x9e3779b97f4a7c15ull);

                for (size_t i = 0; i < kWarmupPairsPerThread; i++) {
                    const auto span_id = static_cast<int64_t>(splitmix64(rng_state) >> 1);
                    auto& handle = ring[i % kHandleRingSize];
                    registry.add(handle, span_id, kBaseStartTime);
                    registry.drop(handle, span_id);
                }

                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                for (size_t i = 0; i < pairs_per_thread; i++) {
                    const auto span_id = static_cast<int64_t>(splitmix64(rng_state) >> 1);
                    auto& handle = ring[i % kHandleRingSize];
                    if (i % kLatencySampleStride == 0) {
                        const auto begin = Clock::now();
                        registry.add(handle, span_id, kBaseStartTime);
                        registry.drop(handle, span_id);
                        samples.push_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin)
                                .count());
                    } else {
                        registry.add(handle, span_id, kBaseStartTime);
                        registry.drop(handle, span_id);
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
        result.nanoseconds_per_pair =
            static_cast<double>(wall_ns) / static_cast<double>(thread_count * pairs_per_thread);
        if (!merged.empty()) {
            result.latency_p50 = merged[merged.size() / 2];
            result.latency_p99 = merged[(merged.size() * 99) / 100];
            result.latency_max = merged.back();
        }
        return result;
    }

    template <typename Registry>
    int32_t total_active(Registry& registry) {
        int32_t buckets[4]{};
        registry.collect(buckets, kBaseStartTime + 100);
        return buckets[0] + buckets[1] + buckets[2] + buckets[3];
    }

    // ns per collect() scan with a fixed in-flight population; the snapshot
    // runs once per stat-collect interval, so this is not a hot path — it is
    // here to show the intrusive list did not regress it.
    template <typename Registry>
    double run_snapshot_phase(Registry& registry, size_t scan_count) {
        int32_t buckets[4]{};
        const auto begin = Clock::now();
        for (size_t i = 0; i < scan_count; i++) {
            registry.collect(buckets, kBaseStartTime + 100);
        }
        const auto wall_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
        // Keep the scans observable so the loop cannot be hoisted away.
        if (buckets[0] < 0) {
            std::cerr << "impossible bucket count\n";
        }
        return static_cast<double>(wall_ns) / static_cast<double>(scan_count);
    }

}  // namespace benchmark
}  // namespace pinpoint

int main(int argc, char** argv) {
    using namespace pinpoint;
    using namespace pinpoint::benchmark;

    size_t total_pairs = 2000000;
    if (argc > 1) {
        total_pairs = static_cast<size_t>(std::stoull(argv[1]));
    }

    LegacyMapRegistry map_registry;
    IntrusiveRegistry intrusive_registry;

    // Steady in-flight population, registered up front and held across every
    // phase: production shards are never empty under load, and a populated
    // map keeps its bucket arrays and node bins warm.
    std::vector<LegacyMapRegistry::Handle> map_resident(kResidentSpans);
    std::vector<IntrusiveRegistry::Handle> intrusive_resident(kResidentSpans);
    for (size_t i = 0; i < kResidentSpans; i++) {
        const auto span_id = static_cast<int64_t>(1000000 + i);
        map_registry.add(map_resident[i], span_id, kBaseStartTime);
        intrusive_registry.add(intrusive_resident[i], span_id, kBaseStartTime);
    }

    std::cout << std::fixed << std::setprecision(1)
              << "pairs=" << total_pairs
              << " shards=" << kShardCount
              << " resident-spans=" << kResidentSpans
              << " (one pair = addActiveSpan + dropActiveSpan)\n";

    bool verified = true;
    const size_t thread_counts[] = {1, 4, 8};
    for (const auto thread_count : thread_counts) {
        const size_t pairs_per_thread = std::max<size_t>(total_pairs / thread_count, 1);

        const auto map_result = run_phase(map_registry, thread_count, pairs_per_thread);
        const auto intrusive_result = run_phase(intrusive_registry, thread_count, pairs_per_thread);

        std::cout << "threads=" << thread_count
                  << " map: ns/pair=" << map_result.nanoseconds_per_pair
                  << " p50=" << map_result.latency_p50
                  << " p99=" << map_result.latency_p99
                  << " max=" << map_result.latency_max
                  << " | intrusive: ns/pair=" << intrusive_result.nanoseconds_per_pair
                  << " p50=" << intrusive_result.latency_p50
                  << " p99=" << intrusive_result.latency_p99
                  << " max=" << intrusive_result.latency_max
                  << " | speedup=" << std::setprecision(2)
                  << (map_result.nanoseconds_per_pair / intrusive_result.nanoseconds_per_pair)
                  << "x\n" << std::setprecision(1);

        // Every transient pair must have unregistered; only the resident
        // population may remain. A leak here means a broken registry, which
        // would also invalidate the timing.
        verified = verified &&
                   total_active(map_registry) == static_cast<int32_t>(kResidentSpans) &&
                   total_active(intrusive_registry) == static_cast<int32_t>(kResidentSpans);
    }

    // Snapshot scan with a large population (fresh registries so the
    // per-pair phases above stay unaffected).
    {
        constexpr size_t kSnapshotSpans = 10000;
        constexpr size_t kScanCount = 1000;
        LegacyMapRegistry map_snapshot;
        IntrusiveRegistry intrusive_snapshot;
        std::vector<LegacyMapRegistry::Handle> map_handles(kSnapshotSpans);
        std::vector<IntrusiveRegistry::Handle> intrusive_handles(kSnapshotSpans);
        uint64_t rng_state = 0xfedcba9876543210ull;
        for (size_t i = 0; i < kSnapshotSpans; i++) {
            const auto span_id = static_cast<int64_t>(splitmix64(rng_state) >> 1);
            map_snapshot.add(map_handles[i], span_id, kBaseStartTime);
            intrusive_snapshot.add(intrusive_handles[i], span_id, kBaseStartTime);
        }
        // Random ids can collide in the map (dropping the entry) but never in
        // the intrusive list, so compare each against its own registry.
        const auto map_total = total_active(map_snapshot);
        const auto intrusive_total = total_active(intrusive_snapshot);
        verified = verified && intrusive_total == static_cast<int32_t>(kSnapshotSpans) &&
                   map_total > 0;

        const auto map_scan = run_snapshot_phase(map_snapshot, kScanCount);
        const auto intrusive_scan = run_snapshot_phase(intrusive_snapshot, kScanCount);
        std::cout << "snapshot actives=" << kSnapshotSpans
                  << " map: ns/scan=" << map_scan
                  << " | intrusive: ns/scan=" << intrusive_scan << "\n";
    }

    if (!verified) {
        std::cerr << "active span registry verification failed\n";
        return 1;
    }
    return 0;
}
