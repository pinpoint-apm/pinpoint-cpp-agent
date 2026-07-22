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

// Measures the metadata id cache hit path under thread contention. The api id
// cache is consulted once per sampled span plus once per named span event, so
// its warm hit must not collapse as request threads are added. A single-shard
// cache serializes nothing logically, but every hit is still two atomic RMWs
// (shared-lock acquire/release) on one shared_mutex cache line, which
// ping-pongs between cores; the sharded layout spreads that traffic across
// kDefaultCacheShardCount lines. This benchmark compares the two.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "cache.h"

namespace pinpoint::benchmark {
    using Clock = std::chrono::steady_clock;

    constexpr size_t kCacheSize = 1024;  // matches the agent's caches
    constexpr size_t kHotKeyCount = 64;  // distinct hot operations in flight

    std::vector<std::string> hot_keys() {
        std::vector<std::string> keys;
        keys.reserve(kHotKeyCount);
        for (size_t i = 0; i < kHotKeyCount; ++i) {
            keys.push_back("com.example.OrderService.operation" +
                           std::to_string(i) + "(HttpRequest, HttpResponse)");
        }
        return keys;
    }

    struct OperationResult {
        int32_t value;
        bool found;
        bool matches_expected;
    };

    struct ThreadResult {
        uint64_t checksum{0};
        size_t misses{0};
        size_t mismatches{0};
    };

    struct ParallelResult {
        double nanoseconds_per_operation;
        uint64_t checksum;
        size_t misses;
        size_t mismatches;
    };

    template<typename Operation>
    ParallelResult run_parallel(size_t thread_count,
                                size_t operations_per_thread,
                                Operation&& operation) {
        std::atomic<size_t> ready{0};
        std::atomic<bool> start{false};
        std::vector<ThreadResult> results(thread_count);
        std::vector<std::thread> threads;
        threads.reserve(thread_count);

        for (size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
            threads.emplace_back([&, thread_index] {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                ThreadResult result;
                for (size_t i = 0; i < operations_per_thread; ++i) {
                    const auto operation_result = operation(thread_index, i);
                    result.checksum += static_cast<uint64_t>(operation_result.value);
                    result.misses += operation_result.found ? 0 : 1;
                    result.mismatches += operation_result.matches_expected ? 0 : 1;
                }
                results[thread_index] = result;
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
        size_t misses = 0;
        size_t mismatches = 0;
        for (const auto& result : results) {
            checksum += result.checksum;
            misses += result.misses;
            mismatches += result.mismatches;
        }
        const auto operation_count = thread_count * operations_per_thread;
        const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        return ParallelResult{
            static_cast<double>(nanoseconds) / static_cast<double>(operation_count),
            checksum,
            misses,
            mismatches};
    }
}

int main(int argc, char** argv) {
    using namespace pinpoint;
    using namespace pinpoint::benchmark;

    size_t iterations = 2000000;
    if (argc > 1) {
        iterations = static_cast<size_t>(std::stoull(argv[1]));
    }

    const auto keys = hot_keys();
    const size_t thread_counts[] = {1, 4, 8};

    // Warm both caches identically (single-threaded, same key order), so both
    // assign the same ids and every measured operation is a hit. Each thread
    // walks the hot set from its own offset — the shard a key maps to is a
    // property of the key, so all configurations see the same key stream.
    ApiIdCache single_shard(kCacheSize, 1);
    ApiIdCache sharded(kCacheSize, kDefaultCacheShardCount);
    std::vector<int32_t> expected_ids;
    expected_ids.reserve(keys.size());
    for (const auto& key : keys) {
        const auto single = single_shard.get(ApiCacheKey{key, 100});
        const auto shard = sharded.get(ApiCacheKey{key, 100});
        if (single.found || shard.found || single.value != shard.value) {
            std::cerr << "id cache warm-up verification failed\n";
            return 1;
        }
        expected_ids.push_back(single.value);
    }

    std::cout << std::fixed << std::setprecision(2)
              << "iterations=" << iterations
              << " hot-keys=" << kHotKeyCount
              << " shards=" << sharded.shardCount() << '\n';

    bool verified = true;
    for (const auto thread_count : thread_counts) {
        const size_t operations_per_thread =
            std::max<size_t>(iterations / thread_count, 1);

        auto hit = [&keys, &expected_ids](ApiIdCache& cache,
                                         size_t thread_index,
                                         size_t i) {
            const auto key_index = (thread_index + i) % kHotKeyCount;
            const auto& key = keys[key_index];
            const auto result = cache.get(ApiCacheKey{key, 100});
            return OperationResult{
                result.value,
                result.found,
                result.value == expected_ids[key_index]};
        };
        auto single = run_parallel(
            thread_count, operations_per_thread, [&](size_t t, size_t i) {
                return hit(single_shard, t, i);
            });
        auto shard = run_parallel(
            thread_count, operations_per_thread, [&](size_t t, size_t i) {
                return hit(sharded, t, i);
            });

        std::cout << "threads=" << thread_count
                  << " single-shard: ns/op=" << single.nanoseconds_per_operation
                  << " sharded: ns/op=" << shard.nanoseconds_per_operation
                  << " speedup=" << (single.nanoseconds_per_operation /
                                     shard.nanoseconds_per_operation)
                  << "x\n";

        // Validate misses and values directly. Comparing only checksums would
        // let matching miss patterns in both configurations cancel out.
        verified = verified && single.misses == 0 && shard.misses == 0 &&
                   single.mismatches == 0 && shard.mismatches == 0 &&
                   single.checksum == shard.checksum;
    }

    if (!verified) {
        std::cerr << "id cache verification failed\n";
        return 1;
    }
    return 0;
}
