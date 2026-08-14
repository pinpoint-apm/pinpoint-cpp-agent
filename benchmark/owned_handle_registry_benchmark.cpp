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

// A/B for the OwnedHandleRegistry sharding in src/tracer_c.cpp: is the 32-way
// alignas(64) shard array worth its lines, or would one shared_mutex over one
// unordered_map serve the same readers?
//
// The registry lives in an anonymous namespace inside tracer_c.cpp and cannot
// be included, so it is copied here verbatim with the shard count lifted to a
// template parameter. Shards=1 IS the single-mutex candidate — same code, same
// lock type, same map — so the only variable between the columns is the
// sharding itself, not an incidentally different implementation.
//
// Workload mirrors the C API: a handle is inserted once when a span starts,
// looked up on every span-level call (the production comment puts this at 5-15
// times per traced request, from every request thread), and erased at end. Each
// thread only touches handles it created, as a request thread does.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pinpoint {
namespace benchmark {

    using Clock = std::chrono::steady_clock;

    constexpr size_t kFindsPerHandle = 10;     // midpoint of the 5-15 range
    constexpr size_t kResidentHandles = 256;   // steady in-flight population
    constexpr size_t kWarmupCyclesPerThread = 2000;
    constexpr size_t kLatencySampleStride = 64;

    // Stand-in for pt_span_s: the entry is a shared_ptr, so find() copying it
    // costs a control-block refcount increment exactly as in production.
    struct Wrapper {
        int64_t payload{0};
    };

    using Handle = void*;

    // Copied from tracer_c.cpp.
    std::atomic<uintptr_t>& next_owned_handle_id() {
        static auto* next = new std::atomic<uintptr_t>{1};
        return *next;
    }

    Handle make_owned_handle_token() {
        constexpr auto kMaxId = std::numeric_limits<uintptr_t>::max() >> 1;
        auto& next = next_owned_handle_id();
        auto id = next.load(std::memory_order_relaxed);
        while (true) {
            if (id > kMaxId) {
                throw std::overflow_error("C handle token space exhausted");
            }
            if (next.compare_exchange_weak(id, id + 1,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
                return reinterpret_cast<Handle>((id << 1) | uintptr_t{1});
            }
        }
    }

    // Production registry, verbatim apart from the shard count becoming a
    // template parameter. ShardCount=1 is the un-sharded candidate.
    template <size_t ShardCount>
    class OwnedHandleRegistry {
    public:
        using Entry = std::shared_ptr<Wrapper>;

        Handle insert(Entry entry) {
            const auto handle = make_owned_handle_token();
            auto& shard = shard_for(handle);
            std::lock_guard<std::shared_mutex> lock(shard.mutex);
            const auto [pos, inserted] = shard.live.emplace(handle, std::move(entry));
            (void)pos;
            if (!inserted) {
                throw std::logic_error("duplicate C handle token");
            }
            return handle;
        }

        Entry find(Handle handle) {
            auto& shard = shard_for(handle);
            std::shared_lock<std::shared_mutex> lock(shard.mutex);
            const auto found = shard.live.find(handle);
            return found == shard.live.end() ? Entry{} : found->second;
        }

        Entry erase(Handle handle) {
            auto& shard = shard_for(handle);
            Entry removed;
            {
                std::lock_guard<std::shared_mutex> lock(shard.mutex);
                const auto found = shard.live.find(handle);
                if (found == shard.live.end()) {
                    return {};
                }
                removed = std::move(found->second);
                shard.live.erase(found);
            }
            return removed;
        }

        size_t size() {
            size_t total = 0;
            for (auto& shard : shards_) {
                std::shared_lock<std::shared_mutex> lock(shard.mutex);
                total += shard.live.size();
            }
            return total;
        }

    private:
        struct alignas(64) Shard {
            std::shared_mutex mutex;
            std::unordered_map<Handle, Entry> live;
        };

        Shard& shard_for(Handle handle) {
            return shards_[(reinterpret_cast<uintptr_t>(handle) >> 1) % ShardCount];
        }

        Shard shards_[ShardCount];
    };

    struct PhaseResult {
        double nanoseconds_per_cycle{0.0};
        double nanoseconds_per_find{0.0};
        int64_t latency_p50{0};
        int64_t latency_p99{0};
        int64_t latency_max{0};
        bool verified{true};
    };

    // One cycle = insert + kFindsPerHandle finds + erase, i.e. one traced
    // request's worth of registry traffic.
    template <typename Registry>
    PhaseResult run_phase(Registry& registry, size_t thread_count, size_t cycles_per_thread) {
        std::atomic<size_t> ready{0};
        std::atomic<bool> go{false};
        std::atomic<bool> ok{true};
        std::vector<std::vector<int64_t>> sampled(thread_count);
        std::vector<std::thread> threads;
        threads.reserve(thread_count);

        for (size_t t = 0; t < thread_count; t++) {
            threads.emplace_back([&, t] {
                auto& samples = sampled[t];
                samples.reserve(cycles_per_thread / kLatencySampleStride + 1);
                auto entry = std::make_shared<Wrapper>();
                int64_t sink = 0;

                const auto cycle = [&] {
                    const auto handle = registry.insert(entry);
                    for (size_t f = 0; f < kFindsPerHandle; f++) {
                        const auto found = registry.find(handle);
                        if (!found) {
                            ok.store(false, std::memory_order_relaxed);
                        } else {
                            sink += found->payload;
                        }
                    }
                    if (!registry.erase(handle)) {
                        ok.store(false, std::memory_order_relaxed);
                    }
                };

                for (size_t i = 0; i < kWarmupCyclesPerThread; i++) {
                    cycle();
                }

                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                for (size_t i = 0; i < cycles_per_thread; i++) {
                    if (i % kLatencySampleStride == 0) {
                        const auto begin = Clock::now();
                        cycle();
                        samples.push_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin)
                                .count());
                    } else {
                        cycle();
                    }
                }
                if (sink == std::numeric_limits<int64_t>::min()) {
                    std::cerr << "impossible sink\n";  // keeps the finds observable
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
        for (auto& s : sampled) {
            merged.insert(merged.end(), s.begin(), s.end());
        }
        std::sort(merged.begin(), merged.end());

        PhaseResult result;
        const auto cycles = static_cast<double>(thread_count * cycles_per_thread);
        result.nanoseconds_per_cycle = static_cast<double>(wall_ns) / cycles;
        result.nanoseconds_per_find =
            static_cast<double>(wall_ns) / (cycles * static_cast<double>(kFindsPerHandle));
        if (!merged.empty()) {
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
        result.verified = ok.load(std::memory_order_relaxed);
        return result;
    }

    // Read-only phase: the population is pre-inserted and every thread only
    // looks up. Isolates reader contention on the shared_mutex from the
    // exclusive-lock insert/erase traffic.
    template <typename Registry>
    PhaseResult run_find_only_phase(Registry& registry, const std::vector<Handle>& handles,
                                    size_t thread_count, size_t finds_per_thread) {
        std::atomic<size_t> ready{0};
        std::atomic<bool> go{false};
        std::atomic<bool> ok{true};
        std::vector<std::thread> threads;
        threads.reserve(thread_count);

        for (size_t t = 0; t < thread_count; t++) {
            threads.emplace_back([&, t] {
                // Each thread walks its own slice, as a request thread looks
                // up only the handles it created. Both terms are clamped so a
                // thread count above the resident population still indexes in
                // range instead of dividing by a zero stride; for every count
                // at or below it they are the plain slice.
                const size_t stride = std::max<size_t>(handles.size() / thread_count, 1);
                const size_t base = (t * stride) % handles.size();
                int64_t sink = 0;
                for (size_t i = 0; i < 2000; i++) {
                    const auto found = registry.find(handles[base + (i % stride)]);
                    if (found) sink += found->payload;
                }

                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                for (size_t i = 0; i < finds_per_thread; i++) {
                    const auto found = registry.find(handles[base + (i % stride)]);
                    if (!found) {
                        ok.store(false, std::memory_order_relaxed);
                    } else {
                        sink += found->payload;
                    }
                }
                if (sink == std::numeric_limits<int64_t>::min()) {
                    std::cerr << "impossible sink\n";
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

        PhaseResult result;
        result.nanoseconds_per_find =
            static_cast<double>(wall_ns) /
            static_cast<double>(thread_count * finds_per_thread);
        result.verified = ok.load(std::memory_order_relaxed);
        return result;
    }

    template <typename Registry>
    void fill_resident(Registry& registry, std::vector<Handle>& out, size_t count) {
        auto entry = std::make_shared<Wrapper>();
        for (size_t i = 0; i < count; i++) {
            out.push_back(registry.insert(entry));
        }
    }

}  // namespace benchmark
}  // namespace pinpoint

int main(int argc, char** argv) {
    using namespace pinpoint::benchmark;

    size_t total_cycles = 400000;
    if (argc > 1) {
        total_cycles = static_cast<size_t>(std::stoull(argv[1]));
    }

    std::cout << std::fixed << std::setprecision(1)
              << "cycles=" << total_cycles
              << " finds-per-cycle=" << kFindsPerHandle
              << " resident=" << kResidentHandles
              << " (one cycle = insert + " << kFindsPerHandle << " finds + erase)\n\n";

    bool verified = true;

    // Registries are function-scope so each shard-count column starts from the
    // same warmed, resident-populated state.
    OwnedHandleRegistry<1> reg1;
    OwnedHandleRegistry<4> reg4;
    OwnedHandleRegistry<8> reg8;
    OwnedHandleRegistry<32> reg32;

    std::vector<Handle> h1, h4, h8, h32;
    fill_resident(reg1, h1, kResidentHandles);
    fill_resident(reg4, h4, kResidentHandles);
    fill_resident(reg8, h8, kResidentHandles);
    fill_resident(reg32, h32, kResidentHandles);

    std::cout << "== mixed: insert + finds + erase (ns per cycle) ==\n";
    std::cout << "threads   shards=1   shards=4   shards=8  shards=32   1->32\n";
    const size_t thread_counts[] = {1, 4, 8, 14};
    for (const auto threads : thread_counts) {
        const size_t per_thread = std::max<size_t>(total_cycles / threads, 1);
        const auto r1 = run_phase(reg1, threads, per_thread);
        const auto r4 = run_phase(reg4, threads, per_thread);
        const auto r8 = run_phase(reg8, threads, per_thread);
        const auto r32 = run_phase(reg32, threads, per_thread);
        std::cout << std::setw(7) << threads
                  << std::setw(11) << r1.nanoseconds_per_cycle
                  << std::setw(11) << r4.nanoseconds_per_cycle
                  << std::setw(11) << r8.nanoseconds_per_cycle
                  << std::setw(11) << r32.nanoseconds_per_cycle
                  << std::setw(8) << std::setprecision(2)
                  << (r1.nanoseconds_per_cycle / r32.nanoseconds_per_cycle) << "x\n"
                  << std::setprecision(1);
        verified = verified && r1.verified && r4.verified && r8.verified && r32.verified;
    }

    std::cout << "\n== find-only (ns per find) ==\n";
    std::cout << "threads   shards=1   shards=4   shards=8  shards=32   1->32\n";
    for (const auto threads : thread_counts) {
        const size_t per_thread = std::max<size_t>(total_cycles * kFindsPerHandle / threads, 1);
        const auto r1 = run_find_only_phase(reg1, h1, threads, per_thread);
        const auto r4 = run_find_only_phase(reg4, h4, threads, per_thread);
        const auto r8 = run_find_only_phase(reg8, h8, threads, per_thread);
        const auto r32 = run_find_only_phase(reg32, h32, threads, per_thread);
        std::cout << std::setw(7) << threads
                  << std::setw(11) << r1.nanoseconds_per_find
                  << std::setw(11) << r4.nanoseconds_per_find
                  << std::setw(11) << r8.nanoseconds_per_find
                  << std::setw(11) << r32.nanoseconds_per_find
                  << std::setw(8) << std::setprecision(2)
                  << (r1.nanoseconds_per_find / r32.nanoseconds_per_find) << "x\n"
                  << std::setprecision(1);
        verified = verified && r1.verified && r4.verified && r8.verified && r32.verified;
    }

    // Every transient handle must have been erased; only the resident
    // population may remain. A leak would also invalidate the timings.
    verified = verified &&
               reg1.size() == kResidentHandles && reg4.size() == kResidentHandles &&
               reg8.size() == kResidentHandles && reg32.size() == kResidentHandles;

    if (!verified) {
        std::cerr << "owned handle registry verification failed\n";
        return 1;
    }
    return 0;
}
