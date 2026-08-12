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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cache.h"
#include "sql.h"

namespace {
    std::atomic<uint64_t> allocation_count{0};
    std::atomic<bool> count_allocations{false};

    void record_allocation() noexcept {
        if (count_allocations.load(std::memory_order_relaxed)) {
            allocation_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// operator new must return a distinct non-null pointer for a zero-sized
// request, but std::malloc(0) is allowed to return nullptr on some platforms.
// Request at least one byte so a zero-sized allocation cannot spuriously throw.
void* operator new(std::size_t size) {
    record_allocation();
    if (void* memory = std::malloc(size != 0 ? size : 1)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    record_allocation();
    if (void* memory = std::malloc(size != 0 ? size : 1)) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace pinpoint::benchmark {
    using Clock = std::chrono::steady_clock;

    constexpr std::string_view kRawSql =
        "SELECT o.id, o.created_at, c.name FROM orders o JOIN customers c "
        "ON c.id = o.customer_id WHERE o.customer_id = 1042 AND "
        "o.status = 'READY' ORDER BY o.created_at DESC LIMIT 100";

    struct Result {
        double nanoseconds_per_operation;
        double allocations_per_operation;
        uint64_t checksum;
    };

    struct ParallelResult {
        double nanoseconds_per_operation;
        uint64_t checksum;
    };

    template<typename Operation>
    Result run(size_t iterations, Operation&& operation) {
        allocation_count.store(0, std::memory_order_relaxed);
        count_allocations.store(true, std::memory_order_release);
        const auto start = Clock::now();
        uint64_t checksum = 0;
        for (size_t i = 0; i < iterations; ++i) {
            checksum += operation();
        }
        const auto elapsed = Clock::now() - start;
        count_allocations.store(false, std::memory_order_release);

        const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        return Result{
            static_cast<double>(nanoseconds) / static_cast<double>(iterations),
            static_cast<double>(allocation_count.load(std::memory_order_relaxed)) /
                static_cast<double>(iterations),
            checksum};
    }

    template<typename Operation>
    ParallelResult run_parallel(size_t thread_count,
                                size_t operations_per_thread,
                                Operation&& operation) {
        std::atomic<size_t> ready{0};
        std::atomic<bool> start{false};
        std::vector<uint64_t> checksums(thread_count, 0);
        std::vector<std::thread> threads;
        threads.reserve(thread_count);

        for (size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
            threads.emplace_back([&, thread_index] {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                uint64_t checksum = 0;
                for (size_t i = 0; i < operations_per_thread; ++i) {
                    checksum += operation();
                }
                checksums[thread_index] = checksum;
            });
        }

        while (ready.load(std::memory_order_acquire) != thread_count) {
            std::this_thread::yield();
        }
        const auto begin = Clock::now();
        start.store(true, std::memory_order_release);
        for (auto& thread : threads) {
            thread.join();
        }
        const auto elapsed = Clock::now() - begin;

        uint64_t checksum = 0;
        for (const auto value : checksums) {
            checksum += value;
        }
        const auto operation_count = thread_count * operations_per_thread;
        const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        return ParallelResult{
            static_cast<double>(nanoseconds) / static_cast<double>(operation_count),
            checksum};
    }
}

int main(int argc, char** argv) {
    using namespace pinpoint;
    using namespace pinpoint::benchmark;

    size_t iterations = 500000;
    if (argc > 1) {
        iterations = static_cast<size_t>(std::stoull(argv[1]));
    }

    SqlNormalizer normalizer(64 * 1024);
    IdCache canonical_cache(1024);
    auto warm_normalized = normalizer.normalize(kRawSql);
    canonical_cache.get(warm_normalized.normalized_sql);

    auto baseline = run(iterations, [&] {
        auto normalized = normalizer.normalize(kRawSql);
        const auto id = canonical_cache.get(normalized.normalized_sql).value;
        return static_cast<uint64_t>(id) + normalized.parameters.size();
    });

    RawSqlCache raw_cache(1024, 16);
    std::atomic<uint64_t> factory_calls{0};
    auto factory = [&]() -> PreparedSqlRef {
        factory_calls.fetch_add(1, std::memory_order_relaxed);
        auto normalized = normalizer.normalize(kRawSql);
        const auto id = canonical_cache.get(normalized.normalized_sql).value;
        return std::make_shared<const PreparedSql>(PreparedSql{
            std::move(normalized.parameters),
            SqlIdentity{id}});
    };
    raw_cache.get(kRawSql, 0, factory);
    factory_calls.store(0, std::memory_order_relaxed);

    auto cached = run(iterations, [&] {
        auto result = raw_cache.get(kRawSql, 0, factory);
        return static_cast<uint64_t>(std::get<int32_t>(result.value->identity)) +
               result.value->parameters.size();
    });

    constexpr size_t parallel_threads = 8;
    const size_t operations_per_thread =
        std::max<size_t>(iterations / parallel_threads, 1);
    auto baseline_parallel = run_parallel(
        parallel_threads, operations_per_thread, [&] {
            auto normalized = normalizer.normalize(kRawSql);
            const auto id = canonical_cache.get(normalized.normalized_sql).value;
            return static_cast<uint64_t>(id) + normalized.parameters.size();
        });
    auto cached_parallel = run_parallel(
        parallel_threads, operations_per_thread, [&] {
            auto result = raw_cache.get(kRawSql, 0, factory);
            return static_cast<uint64_t>(std::get<int32_t>(result.value->identity)) +
                   result.value->parameters.size();
        });

    // Miss-heavy scenario: a pool of distinct raw statements larger than the
    // cache, so every raw lookup misses and pays the hash + insert + eviction
    // churn on top of normalization. They all normalize to one canonical SQL,
    // so the canonical lookup still hits and only the raw-cache overhead is
    // isolated. This is the worst case for workloads that inline literals
    // instead of using bind parameters, where the raw-cache hit rate is ~0 and
    // the front cache is pure overhead rather than a speedup.
    constexpr size_t miss_pool_size = 4096;
    std::vector<std::string> miss_pool;
    miss_pool.reserve(miss_pool_size);
    for (size_t i = 0; i < miss_pool_size; ++i) {
        miss_pool.push_back(
            "SELECT o.id FROM orders o WHERE o.customer_id = " +
            std::to_string(i));
    }

    size_t miss_index = 0;
    auto baseline_miss = run(iterations, [&] {
        const auto& raw = miss_pool[miss_index++ % miss_pool.size()];
        auto normalized = normalizer.normalize(raw);
        const auto id = canonical_cache.get(normalized.normalized_sql).value;
        return static_cast<uint64_t>(id) + normalized.parameters.size();
    });

    miss_index = 0;
    auto cached_miss = run(iterations, [&] {
        const auto& raw = miss_pool[miss_index++ % miss_pool.size()];
        auto result = raw_cache.get(raw, 0, [&]() -> PreparedSqlRef {
            auto normalized = normalizer.normalize(raw);
            const auto id = canonical_cache.get(normalized.normalized_sql).value;
            return std::make_shared<const PreparedSql>(PreparedSql{
                std::move(normalized.parameters),
                SqlIdentity{id}});
        });
        return static_cast<uint64_t>(std::get<int32_t>(result.value->identity)) +
               result.value->parameters.size();
    });
    const auto miss_overhead = cached_miss.nanoseconds_per_operation /
                               baseline_miss.nanoseconds_per_operation;

    const auto speedup = baseline.nanoseconds_per_operation /
                         cached.nanoseconds_per_operation;
    const auto parser_calls_on_hit = factory_calls.load(std::memory_order_relaxed);
    const auto parallel_speedup = baseline_parallel.nanoseconds_per_operation /
                                  cached_parallel.nanoseconds_per_operation;

    std::cout << std::fixed << std::setprecision(2)
              << "iterations=" << iterations << '\n'
              << "legacy-normalize: ns/op=" << baseline.nanoseconds_per_operation
              << " allocations/op=" << baseline.allocations_per_operation << '\n'
              << "raw-cache-hit:   ns/op=" << cached.nanoseconds_per_operation
              << " allocations/op=" << cached.allocations_per_operation
              << " parser-calls=" << parser_calls_on_hit << '\n'
              << "speedup=" << speedup << "x\n"
              << "parallel(" << parallel_threads << " threads) legacy: ns/op="
              << baseline_parallel.nanoseconds_per_operation << '\n'
              << "parallel(" << parallel_threads << " threads) raw-hit: ns/op="
              << cached_parallel.nanoseconds_per_operation
              << " speedup=" << parallel_speedup << "x\n"
              << "miss (distinct raw) legacy:   ns/op="
              << baseline_miss.nanoseconds_per_operation
              << " allocations/op=" << baseline_miss.allocations_per_operation << '\n'
              << "miss (distinct raw) raw-cache: ns/op="
              << cached_miss.nanoseconds_per_operation
              << " allocations/op=" << cached_miss.allocations_per_operation
              << " overhead=" << miss_overhead << "x (>1 means the front cache"
              << " costs more than it saves on this workload)\n";

    // Keep both loops observable under optimization and turn the benchmark
    // into an automated verification of the two hard hot-path requirements.
    if (baseline.checksum != cached.checksum ||
        baseline_parallel.checksum != cached_parallel.checksum ||
        baseline_miss.checksum != cached_miss.checksum ||
        cached.allocations_per_operation != 0.0 ||
        parser_calls_on_hit != 0) {
        std::cerr << "raw SQL cache verification failed\n";
        return 1;
    }
    return 0;
}
