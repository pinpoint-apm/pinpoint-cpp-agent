/*
 * Copyright 2020-present NAVER Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "atomic_shared_ptr.h"

namespace pinpoint::benchmark {
namespace {

class LegacyAtomicSharedPtr {
public:
    explicit LegacyAtomicSharedPtr(std::shared_ptr<const uint64_t> value)
        : value_(std::move(value)) {}

    std::shared_ptr<const uint64_t> load() const {
        std::shared_lock lock(mutex_);
        return value_;
    }

private:
    mutable std::shared_mutex mutex_;
    std::shared_ptr<const uint64_t> value_;
};

struct Result {
    double contended_ns_per_op;
    double aggregate_mops;
};

std::atomic<uint64_t> benchmark_sink{0};

template <typename Prepare, typename Load>
Result run_once(size_t thread_count, uint64_t operations, Prepare prepare, Load load) {
    std::atomic<size_t> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (size_t i = 0; i < thread_count; ++i) {
        workers.emplace_back([&] {
            prepare();
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            uint64_t checksum = 0;
            for (uint64_t op = 0; op < operations; ++op) {
                checksum += load();
            }
            benchmark_sink.fetch_add(checksum, std::memory_order_relaxed);
        });
    }

    while (ready.load(std::memory_order_acquire) != thread_count) {
        std::this_thread::yield();
    }
    const auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }
    const auto end = std::chrono::steady_clock::now();

    const double elapsed_ns =
        std::chrono::duration<double, std::nano>(end - begin).count();
    return {elapsed_ns / static_cast<double>(operations),
            static_cast<double>(thread_count * operations) * 1000.0 / elapsed_ns};
}

template <typename Prepare, typename Load>
Result run_median(size_t thread_count, uint64_t operations, Prepare prepare, Load load) {
    std::vector<Result> results;
    results.reserve(3);
    for (int repeat = 0; repeat < 3; ++repeat) {
        results.push_back(run_once(thread_count, operations, prepare, load));
    }
    std::sort(results.begin(), results.end(), [](const Result& lhs, const Result& rhs) {
        return lhs.contended_ns_per_op < rhs.contended_ns_per_op;
    });
    return results[results.size() / 2];
}

std::vector<size_t> thread_counts() {
    const size_t detected = std::max(1u, std::thread::hardware_concurrency());
    const size_t limit = std::min<size_t>(detected, 16);
    std::vector<size_t> counts;
    for (size_t count = 1; count <= limit; count *= 2) {
        counts.push_back(count);
    }
    if (counts.back() != limit) {
        counts.push_back(limit);
    }
    return counts;
}

uint64_t parse_operations(int argc, char** argv) {
    if (argc == 1) {
        return 1000000;
    }
    if (argc != 2) {
        throw std::invalid_argument("expected at most one operations-per-thread argument");
    }
    // strtoull alone would accept trailing garbage ("12abc" -> 12) and wrap
    // negative input ("-1" -> huge), so validate the whole token.
    char* end = nullptr;
    errno = 0;
    const auto operations = std::strtoull(argv[1], &end, 10);
    if (argv[1][0] == '-' || end == argv[1] || *end != '\0' || errno == ERANGE ||
        operations == 0) {
        throw std::invalid_argument(
            "operations per thread must be a positive decimal integer");
    }
    return operations;
}

}  // namespace

int run(int argc, char** argv) {
    const uint64_t operations = parse_operations(argc, argv);
    LegacyAtomicSharedPtr legacy(std::make_shared<const uint64_t>(7));
    AtomicSharedPtr<const uint64_t, SnapshotCache::ThreadCached> cached(
        std::make_shared<const uint64_t>(7));

    std::cout << "AtomicSharedPtr unchanged-value load benchmark\n"
              << "operations/thread=" << operations << ", median of 3\n"
              << "ns/op is elapsed wall time divided by per-thread operations; "
                 "stable values mean contention does not grow with thread count.\n\n"
              << std::setw(7) << "threads"
              << std::setw(18) << "legacy ns/op"
              << std::setw(18) << "cached load"
              << std::setw(18) << "cached ref"
              << std::setw(14) << "ref speedup" << '\n';

    for (const size_t threads : thread_counts()) {
        const auto legacy_result = run_median(
            threads, operations, [] {}, [&] { return *legacy.load(); });
        const auto cached_value_result = run_median(
            threads, operations,
            [&] { (void)cached.load_cached_ref(); },
            [&] { return *cached.load(); });
        const auto cached_ref_result = run_median(
            threads, operations,
            [&] { (void)cached.load_cached_ref(); },
            [&] { return *cached.load_cached_ref(); });

        std::cout << std::fixed << std::setprecision(2)
                  << std::setw(7) << threads
                  << std::setw(18) << legacy_result.contended_ns_per_op
                  << std::setw(18) << cached_value_result.contended_ns_per_op
                  << std::setw(18) << cached_ref_result.contended_ns_per_op
                  << std::setw(13)
                  << legacy_result.contended_ns_per_op /
                         cached_ref_result.contended_ns_per_op
                  << "x\n";
    }

    std::cout << "\nchecksum="
              << benchmark_sink.load(std::memory_order_relaxed) << '\n';
    return 0;
}

}  // namespace pinpoint::benchmark

int main(int argc, char** argv) {
    try {
        return pinpoint::benchmark::run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "atomic_shared_ptr_benchmark: " << error.what() << '\n';
        return 1;
    }
}
