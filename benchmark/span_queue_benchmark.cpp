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
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sharded_bounded_queue.h"

namespace pinpoint {
namespace benchmark {

    using Clock = std::chrono::steady_clock;

    constexpr double kRequiredThroughputSpeedup = 2.0;
    constexpr double kRequiredP99ReductionPercent = 70.0;
    constexpr double kAllowedSingleProducerRegressionPercent = 5.0;

    std::atomic<uint64_t> benchmark_sink{0};

    // Allocation-free, move-only token with the same size and ownership
    // transfer operations as the unique_ptr stored by the real span queue.
    class QueueValue final {
    public:
        QueueValue() noexcept = default;
        explicit QueueValue(uint64_t id) noexcept : id_(id) {}

        QueueValue(const QueueValue&) = delete;
        QueueValue& operator=(const QueueValue&) = delete;

        QueueValue(QueueValue&& other) noexcept
            : id_(std::exchange(other.id_, 0)) {}

        QueueValue& operator=(QueueValue&& other) noexcept {
            id_ = std::exchange(other.id_, 0);
            return *this;
        }

        uint64_t id() const noexcept { return id_; }

    private:
        uint64_t id_{0};
    };

    static_assert(sizeof(QueueValue) == sizeof(std::unique_ptr<int>));

    class LegacyMutexQueue final {
    public:
        explicit LegacyMutexQueue(size_t capacity) : capacity_(capacity) {}

        bool enqueue(QueueValue& value) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.size() >= capacity_) {
                    queue_.pop();
                    ++dropped_oldest_;
                }
                queue_.push(std::move(value));
            }
            // The old GrpcSpan path notified on every enqueue, even while its
            // worker was active and not waiting for queue data.
            cv_.notify_one();
            return true;
        }

        bool try_dequeue(QueueValue& value) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                return false;
            }
            value = std::move(queue_.front());
            queue_.pop();
            return true;
        }

        uint64_t dropped_oldest() const noexcept { return dropped_oldest_; }
    private:
        const size_t capacity_;
        std::queue<QueueValue> queue_;
        std::mutex mutex_;
        std::condition_variable cv_;
        uint64_t dropped_oldest_{0};
    };

    class ShardedSpanQueue final {
    public:
        explicit ShardedSpanQueue(size_t capacity) : queue_(capacity) {}

        bool enqueue(QueueValue& value) {
            queue_.enqueue(value);
            notify_if_waiting();
            return true;
        }

        bool try_dequeue(QueueValue& value) {
            return queue_.try_dequeue(value);
        }

        uint64_t dropped_oldest() const {
            return queue_.dropped_oldest();
        }

    private:
        void notify_if_waiting() {
            if (!consumer_waiting_.load(std::memory_order_acquire)) {
                return;
            }
            std::lock_guard<std::mutex> lock(wait_mutex_);
            if (consumer_waiting_.load(std::memory_order_relaxed)) {
                cv_.notify_one();
            }
        }

        ShardedBoundedQueue<QueueValue> queue_;
        std::atomic<bool> consumer_waiting_{false};
        std::mutex wait_mutex_;
        std::condition_variable cv_;
    };

    enum class ConsumerMode {
        Drain,
        Slow,
        Paused,
    };

    const char* mode_name(ConsumerMode mode) {
        switch (mode) {
            case ConsumerMode::Drain: return "drain";
            case ConsumerMode::Slow: return "slow";
            case ConsumerMode::Paused: return "paused";
        }
        return "unknown";
    }

    struct Options {
        std::vector<size_t> producers{1, 8, 32, 64};
        size_t operations_per_producer{100000};
        size_t repeats{3};
        size_t queue_size{1024};
        size_t sample_every{64};
        std::vector<ConsumerMode> modes{ConsumerMode::Drain};
        bool verify{false};
        bool help{false};
    };

    struct TrialResult {
        double operations_per_second{0};
        uint64_t p50_ns{0};
        uint64_t p99_ns{0};
        uint64_t max_ns{0};
        uint64_t dropped_oldest{0};
        uint64_t checksum{0};
    };

    struct AggregateResult {
        double operations_per_second{0};
        uint64_t p50_ns{0};
        uint64_t p99_ns{0};
        uint64_t max_ns{0};
        uint64_t dropped_oldest{0};
    };

    struct ScenarioResult {
        ConsumerMode mode;
        size_t producers;
        AggregateResult legacy;
        AggregateResult concurrent;
    };

    uint64_t percentile(const std::vector<uint64_t>& sorted, double fraction) {
        if (sorted.empty()) {
            return 0;
        }
        const size_t index = static_cast<size_t>(
            fraction * static_cast<double>(sorted.size() - 1));
        return sorted[index];
    }

    template <typename Queue>
    TrialResult run_trial(size_t producer_count,
                          size_t operations_per_producer,
                          size_t queue_size,
                          size_t sample_every,
                          ConsumerMode mode) {
        Queue queue(queue_size);
        std::atomic<bool> start{false};
        std::atomic<bool> producers_done{false};
        std::atomic<size_t> ready_producers{0};
        std::atomic<bool> consumer_ready{mode == ConsumerMode::Paused};

        uint64_t consumer_checksum = 0;
        std::thread consumer;
        if (mode != ConsumerMode::Paused) {
            consumer = std::thread([&] {
                consumer_ready.store(true, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                QueueValue value;
                while (true) {
                    if (queue.try_dequeue(value)) {
                        consumer_checksum ^= value.id() + 0x9e3779b97f4a7c15ULL +
                                             (consumer_checksum << 6) +
                                             (consumer_checksum >> 2);
                        if (mode == ConsumerMode::Slow) {
                            for (size_t spin = 0; spin < 64; ++spin) {
                                consumer_checksum = (consumer_checksum << 1) ^
                                                    (consumer_checksum >> 3) ^ spin;
                            }
                        }
                        continue;
                    }
                    if (producers_done.load(std::memory_order_acquire)) {
                        break;
                    }
                    std::this_thread::yield();
                }
            });
        }

        std::vector<std::vector<uint64_t>> samples(producer_count);
        std::vector<std::thread> producers;
        producers.reserve(producer_count);
        for (size_t producer = 0; producer < producer_count; ++producer) {
            producers.emplace_back([&, producer] {
                auto& local_samples = samples[producer];
                local_samples.reserve(operations_per_producer / sample_every + 1);
                ready_producers.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                for (size_t operation = 0; operation < operations_per_producer; ++operation) {
                    QueueValue value((static_cast<uint64_t>(producer + 1) << 48) |
                                     static_cast<uint64_t>(operation + 1));
                    if (operation % sample_every == 0) {
                        const auto before = Clock::now();
                        queue.enqueue(value);
                        const auto after = Clock::now();
                        local_samples.push_back(static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                after - before).count()));
                    } else {
                        queue.enqueue(value);
                    }
                }
            });
        }

        while (ready_producers.load(std::memory_order_acquire) < producer_count ||
               !consumer_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        const auto started_at = Clock::now();
        start.store(true, std::memory_order_release);
        for (auto& producer : producers) {
            producer.join();
        }
        const auto producers_finished_at = Clock::now();
        producers_done.store(true, std::memory_order_release);
        if (consumer.joinable()) {
            consumer.join();
        }

        std::vector<uint64_t> combined_samples;
        size_t sample_count = 0;
        for (const auto& producer_samples : samples) {
            sample_count += producer_samples.size();
        }
        combined_samples.reserve(sample_count);
        for (auto& producer_samples : samples) {
            combined_samples.insert(combined_samples.end(),
                                    producer_samples.begin(), producer_samples.end());
        }
        std::sort(combined_samples.begin(), combined_samples.end());

        const auto elapsed_seconds =
            std::chrono::duration<double>(producers_finished_at - started_at).count();
        const auto operation_count = producer_count * operations_per_producer;

        TrialResult result;
        result.operations_per_second = static_cast<double>(operation_count) / elapsed_seconds;
        result.p50_ns = percentile(combined_samples, 0.50);
        result.p99_ns = percentile(combined_samples, 0.99);
        result.max_ns = combined_samples.empty() ? 0 : combined_samples.back();
        result.dropped_oldest = queue.dropped_oldest();
        result.checksum = consumer_checksum;
        benchmark_sink.fetch_xor(result.checksum, std::memory_order_relaxed);
        return result;
    }

    template <typename Value, typename Projection>
    Value median_value(const std::vector<TrialResult>& trials, Projection projection) {
        std::vector<Value> values;
        values.reserve(trials.size());
        for (const auto& trial : trials) {
            values.push_back(static_cast<Value>(projection(trial)));
        }
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

    AggregateResult aggregate(const std::vector<TrialResult>& trials) {
        AggregateResult result;
        result.operations_per_second = median_value<double>(
            trials, [](const auto& trial) { return trial.operations_per_second; });
        result.p50_ns = median_value<uint64_t>(
            trials, [](const auto& trial) { return trial.p50_ns; });
        result.p99_ns = median_value<uint64_t>(
            trials, [](const auto& trial) { return trial.p99_ns; });
        result.max_ns = 0;
        for (const auto& trial : trials) {
            result.max_ns = std::max(result.max_ns, trial.max_ns);
        }
        result.dropped_oldest = median_value<uint64_t>(
            trials, [](const auto& trial) { return trial.dropped_oldest; });
        return result;
    }

    std::vector<size_t> parse_producer_list(const std::string& value) {
        std::vector<size_t> result;
        size_t begin = 0;
        while (begin < value.size()) {
            const auto end = value.find(',', begin);
            const auto token = value.substr(begin, end == std::string::npos
                                                       ? std::string::npos
                                                       : end - begin);
            const auto parsed = static_cast<size_t>(std::stoull(token));
            if (parsed == 0) {
                throw std::invalid_argument("producer counts must be positive");
            }
            result.push_back(parsed);
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
        if (result.empty()) {
            throw std::invalid_argument("producer list must not be empty");
        }
        return result;
    }

    std::vector<ConsumerMode> parse_modes(const std::string& value) {
        if (value == "drain") return {ConsumerMode::Drain};
        if (value == "slow") return {ConsumerMode::Slow};
        if (value == "paused") return {ConsumerMode::Paused};
        if (value == "all") {
            return {ConsumerMode::Drain, ConsumerMode::Slow, ConsumerMode::Paused};
        }
        throw std::invalid_argument("mode must be drain, slow, paused, or all");
    }

    size_t positive_number(const std::string& value, const char* name) {
        const auto parsed = static_cast<size_t>(std::stoull(value));
        if (parsed == 0) {
            throw std::invalid_argument(std::string(name) + " must be positive");
        }
        return parsed;
    }

    Options parse_options(int argc, char** argv) {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            const auto next_value = [&](const char* name) -> std::string {
                if (i + 1 >= argc) {
                    throw std::invalid_argument(std::string(name) + " requires a value");
                }
                return argv[++i];
            };

            if (argument == "--producers") {
                options.producers = parse_producer_list(next_value("--producers"));
            } else if (argument == "--operations") {
                options.operations_per_producer = positive_number(
                    next_value("--operations"), "operations");
            } else if (argument == "--repeats") {
                options.repeats = positive_number(next_value("--repeats"), "repeats");
            } else if (argument == "--queue-size") {
                options.queue_size = positive_number(
                    next_value("--queue-size"), "queue size");
            } else if (argument == "--sample-every") {
                options.sample_every = positive_number(
                    next_value("--sample-every"), "sample interval");
            } else if (argument == "--mode") {
                options.modes = parse_modes(next_value("--mode"));
            } else if (argument == "--verify") {
                options.verify = true;
            } else if (argument == "--help" || argument == "-h") {
                options.help = true;
            } else {
                throw std::invalid_argument("unknown option: " + argument);
            }
        }
        return options;
    }

    void print_usage(const char* program) {
        std::cout
            << "Usage: " << program << " [options]\n"
            << "  --producers LIST     producer counts (default: 1,8,32,64)\n"
            << "  --operations N       operations per producer/repeat (default: 100000)\n"
            << "  --repeats N          repetitions; median is reported (default: 3)\n"
            << "  --queue-size N       bounded queue capacity (default: 1024)\n"
            << "  --sample-every N     latency sampling interval (default: 64)\n"
            << "  --mode MODE          drain, slow, paused, or all (default: drain)\n"
            << "  --verify             fail if performance completion gates are missed\n";
    }

    void print_result_row(ConsumerMode mode, size_t producers,
                          const char* implementation, const AggregateResult& result) {
        std::cout << std::left << std::setw(9) << mode_name(mode)
                  << std::right << std::setw(5) << producers << "  "
                  << std::left << std::setw(10) << implementation
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(11) << result.operations_per_second / 1'000'000.0
                  << std::setw(10) << result.p50_ns
                  << std::setw(10) << result.p99_ns
                  << std::setw(12) << result.max_ns
                  << std::setw(13) << result.dropped_oldest << '\n';
    }

    const ScenarioResult* find_scenario(const std::vector<ScenarioResult>& scenarios,
                                        ConsumerMode mode, size_t producers) {
        for (const auto& scenario : scenarios) {
            if (scenario.mode == mode && scenario.producers == producers) {
                return &scenario;
            }
        }
        return nullptr;
    }

    bool verify_results(const std::vector<ScenarioResult>& scenarios) {
        const auto* single = find_scenario(scenarios, ConsumerMode::Drain, 1);
        const auto* contended = find_scenario(scenarios, ConsumerMode::Drain, 32);
        if (single == nullptr || contended == nullptr) {
            std::cerr << "--verify requires drain mode and producer counts 1 and 32\n";
            return false;
        }

        const double throughput_speedup =
            contended->concurrent.operations_per_second /
            contended->legacy.operations_per_second;
        const double p99_reduction = contended->legacy.p99_ns == 0
            ? 0.0
            : (1.0 - static_cast<double>(contended->concurrent.p99_ns) /
                         static_cast<double>(contended->legacy.p99_ns)) * 100.0;
        const double single_regression =
            (1.0 - single->concurrent.operations_per_second /
                       single->legacy.operations_per_second) * 100.0;

        const bool throughput_ok = throughput_speedup >= kRequiredThroughputSpeedup;
        const bool p99_ok = p99_reduction >= kRequiredP99ReductionPercent;
        const bool single_ok = single_regression <= kAllowedSingleProducerRegressionPercent;

        std::cout << "\nCompletion gates (drain mode)\n"
                  << "  [" << (throughput_ok ? "PASS" : "FAIL")
                  << "] 32-producer throughput: " << std::fixed << std::setprecision(2)
                  << throughput_speedup << "x (required >= "
                  << kRequiredThroughputSpeedup << "x)\n"
                  << "  [" << (p99_ok ? "PASS" : "FAIL")
                  << "] 32-producer p99 reduction: " << p99_reduction
                  << "% (required >= " << kRequiredP99ReductionPercent << "%)\n"
                  << "  [" << (single_ok ? "PASS" : "FAIL")
                  << "] 1-producer throughput regression: " << single_regression
                  << "% (allowed <= " << kAllowedSingleProducerRegressionPercent << "%)\n";
        return throughput_ok && p99_ok && single_ok;
    }

    int run(const Options& options) {
        std::cout << "Span queue microbenchmark (optimized target)\n"
                  << "queue_size=" << options.queue_size
                  << " operations_per_producer=" << options.operations_per_producer
                  << " repeats=" << options.repeats
                  << " sample_every=" << options.sample_every << "\n\n"
                  << std::left << std::setw(9) << "mode"
                  << std::right << std::setw(5) << "prod" << "  "
                  << std::left << std::setw(10) << "queue"
                  << std::right << std::setw(11) << "Mops/s"
                  << std::setw(10) << "p50(ns)"
                  << std::setw(10) << "p99(ns)"
                  << std::setw(12) << "max(ns)"
                  << std::setw(13) << "drop-old" << '\n';

        std::vector<ScenarioResult> scenarios;
        for (const auto mode : options.modes) {
            for (const auto producer_count : options.producers) {
                std::vector<TrialResult> legacy_trials;
                std::vector<TrialResult> concurrent_trials;
                legacy_trials.reserve(options.repeats);
                concurrent_trials.reserve(options.repeats);

                // Alternate order to reduce systematic thermal/order bias.
                for (size_t repeat = 0; repeat < options.repeats; ++repeat) {
                    if (repeat % 2 == 0) {
                        legacy_trials.push_back(run_trial<LegacyMutexQueue>(
                            producer_count, options.operations_per_producer,
                            options.queue_size, options.sample_every, mode));
                        concurrent_trials.push_back(run_trial<ShardedSpanQueue>(
                            producer_count, options.operations_per_producer,
                            options.queue_size, options.sample_every, mode));
                    } else {
                        concurrent_trials.push_back(run_trial<ShardedSpanQueue>(
                            producer_count, options.operations_per_producer,
                            options.queue_size, options.sample_every, mode));
                        legacy_trials.push_back(run_trial<LegacyMutexQueue>(
                            producer_count, options.operations_per_producer,
                            options.queue_size, options.sample_every, mode));
                    }
                }

                ScenarioResult scenario{mode, producer_count,
                                        aggregate(legacy_trials),
                                        aggregate(concurrent_trials)};
                print_result_row(mode, producer_count, "mutex", scenario.legacy);
                print_result_row(mode, producer_count, "sharded", scenario.concurrent);
                scenarios.push_back(scenario);
            }
        }

        std::cout << "\nchecksum=" << benchmark_sink.load(std::memory_order_relaxed) << '\n';
        if (options.verify && !verify_results(scenarios)) {
            return 2;
        }
        return 0;
    }

} // namespace benchmark
} // namespace pinpoint

int main(int argc, char** argv) {
    try {
        const auto options = pinpoint::benchmark::parse_options(argc, argv);
        if (options.help) {
            pinpoint::benchmark::print_usage(argv[0]);
            return 0;
        }
        return pinpoint::benchmark::run(options);
    } catch (const std::exception& error) {
        std::cerr << "span_queue_benchmark: " << error.what() << '\n';
        pinpoint::benchmark::print_usage(argv[0]);
        return 1;
    }
}
