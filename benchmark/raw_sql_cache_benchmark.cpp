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

void* operator new(std::size_t size) {
    record_allocation();
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    record_allocation();
    if (void* memory = std::malloc(size)) {
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
            std::move(normalized.normalized_sql),
            std::move(normalized.parameters),
            SqlIdentity{id},
            0});
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
              << " speedup=" << parallel_speedup << "x\n";

    // Keep both loops observable under optimization and turn the benchmark
    // into an automated verification of the two hard hot-path requirements.
    if (baseline.checksum != cached.checksum ||
        baseline_parallel.checksum != cached_parallel.checksum ||
        cached.allocations_per_operation != 0.0 ||
        parser_calls_on_hit != 0) {
        std::cerr << "raw SQL cache verification failed\n";
        return 1;
    }
    return 0;
}
