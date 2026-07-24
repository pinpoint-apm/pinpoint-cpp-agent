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

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "config.h"
#include "agent_service.h"
#include "utility.h"

namespace pinpoint {
    constexpr int URL_STATS_BUCKET_SIZE      = 8;
    constexpr int URL_STATS_BUCKET_VERSION   = 0;

    // Production intervals; UrlStats accepts overrides so tests can drive the
    // tick bucketing and the periodic send in milliseconds instead of 30s.
    constexpr auto URL_STAT_TICK_INTERVAL = std::chrono::seconds(30);
    constexpr auto URL_STAT_SEND_INTERVAL = std::chrono::seconds(30);

    /**
     * @brief Maintains a fixed interval clock used to bucketize URL statistics.
     */
    class TickClock {
    public:
        explicit TickClock(int64_t interval) : interval_(interval) {}
        /**
         * @brief Returns the tick for the supplied end time.
         *
         * @param end_time Span completion time.
         */
        int64_t tick(std::chrono::system_clock::time_point end_time) const;

    private:
        int64_t interval_;
    };

    /**
     * @brief Histogram aggregating elapsed times for URL statistics.
     */
    class UrlStatHistogram {
    public:
        UrlStatHistogram() : total_(0), max_(0) {
            std::fill_n(histogram_, URL_STATS_BUCKET_SIZE, 0);
        }
        ~UrlStatHistogram() = default;

        /**
         * @brief Adds an elapsed time sample to the histogram.
         *
         * @param elapsed Sample duration in milliseconds.
         */
        void add(int32_t elapsed);
        /// @brief Returns the total accumulated elapsed time.
        int64_t total() const { return total_; }
        /// @brief Returns the maximum observed elapsed time.
        int64_t max() const { return max_; }
        /// @brief Returns the bucket value at the specified index.
        int32_t histogram(int index) const { 
            if (index < 0 || index >= URL_STATS_BUCKET_SIZE) {
                return 0;
            }
            return histogram_[index]; 
        }

    private:
        int64_t total_;
        int64_t max_;
        int32_t histogram_[URL_STATS_BUCKET_SIZE]{};
    };


    /**
     * @brief Statistics tracked for a single URL pattern and tick.
     */
    class EachUrlStat {
    public:
        explicit EachUrlStat(int64_t tick) : tickTime_(tick) {}
        ~EachUrlStat() = default;

        /// @brief Returns the histogram aggregating all responses.
        UrlStatHistogram& getTotalHistogram() { return totalHistogram_; }
        const UrlStatHistogram& getTotalHistogram() const { return totalHistogram_; }
        /// @brief Returns the histogram aggregating failed responses.
        UrlStatHistogram& getFailHistogram() { return failedHistogram_; }
        const UrlStatHistogram& getFailHistogram() const { return failedHistogram_; }
        /// @brief Returns the tick value associated with this statistic.
        int64_t tick() const { return tickTime_; }

    private:
        UrlStatHistogram totalHistogram_;
        UrlStatHistogram failedHistogram_;
        int64_t tickTime_;
    };

    /**
     * @brief Key identifying URL statistics by pattern and tick.
     *
     * Stored keys own url_. UrlStatSnapshot uses the private View form only as
     * a synchronous C++17 unordered_map lookup key, so a hit can hash and
     * compare the method/path fragments without first materializing a string.
     */
    struct UrlKey {
        UrlKey(std::string url, int64_t tick)
            : url_{std::move(url)}, tick_{tick} {}

        bool operator==(const UrlKey& other) const noexcept {
            if (tick_ != other.tick_) {
                return false;
            }
            if (view_ == nullptr && other.view_ == nullptr) {
                return url_ == other.url_;
            }
            if (view_ != nullptr && other.view_ == nullptr) {
                return matches_view(other.url_, *view_);
            }
            if (view_ == nullptr && other.view_ != nullptr) {
                return matches_view(url_, *other.view_);
            }

            // Two view keys are not used by UrlStatMap::find(), but retaining
            // the full equivalence relation keeps UrlKey safe as a key type.
            const auto size = url_size();
            if (size != other.url_size()) {
                return false;
            }
            for (size_t i = 0; i < size; ++i) {
                if (url_at(i) != other.url_at(i)) {
                    return false;
                }
            }
            return true;
        }

        std::string url_;
        int64_t tick_;

    private:
        struct View {
            std::string_view method;
            std::string_view path;
            bool method_prefix;
            bool wildcard;
        };

        friend class UrlStatSnapshot;
        friend struct UrlKeyHash;

        UrlKey(const View& view, int64_t tick)
            : url_{}, tick_{tick}, view_{&view} {}

        static UrlKey lookup(const View& view, int64_t tick) {
            return UrlKey{view, tick};
        }

        static size_t view_url_size(const View& view) noexcept {
            return (view.method_prefix ? view.method.size() + 1 : 0)
                + view.path.size() + (view.wildcard ? 1 : 0);
        }

        static bool matches_view(const std::string& owned, const View& view) noexcept {
            if (owned.size() != view_url_size(view)) {
                return false;
            }

            size_t offset = 0;
            const auto matches_part = [&owned, &offset](std::string_view part) {
                const auto begin = owned.begin() + static_cast<std::ptrdiff_t>(offset);
                if (!std::equal(part.begin(), part.end(), begin)) {
                    return false;
                }
                offset += part.size();
                return true;
            };

            if (view.method_prefix) {
                if (!matches_part(view.method) || owned[offset] != ' ') {
                    return false;
                }
                ++offset;
            }
            if (!matches_part(view.path)) {
                return false;
            }
            if (view.wildcard && owned[offset++] != '*') {
                return false;
            }
            return offset == owned.size();
        }

        size_t url_size() const noexcept {
            if (view_ == nullptr) {
                return url_.size();
            }
            return view_url_size(*view_);
        }

        char url_at(size_t index) const noexcept {
            if (view_ == nullptr) {
                return url_[index];
            }
            if (view_->method_prefix) {
                if (index < view_->method.size()) {
                    return view_->method[index];
                }
                index -= view_->method.size();
                if (index == 0) {
                    return ' ';
                }
                --index;
            }
            if (index < view_->path.size()) {
                return view_->path[index];
            }
            return view_->wildcard ? '*' : '\0';
        }

        size_t url_hash() const noexcept {
            // FNV-1a can be continued across the non-contiguous method/path
            // fragments, unlike std::hash<std::string>. Both owning and view
            // forms pass the exact same logical byte sequence through it.
            std::uint64_t hash = 14695981039346656037ULL;
            const auto append = [&hash](std::string_view part) {
                for (const char c : part) {
                    hash ^= static_cast<unsigned char>(c);
                    hash *= 1099511628211ULL;
                }
            };

            if (view_ == nullptr) {
                append(url_);
            } else {
                if (view_->method_prefix) {
                    append(view_->method);
                    append(" ");
                }
                append(view_->path);
                if (view_->wildcard) {
                    append("*");
                }
            }
            return static_cast<size_t>(hash);
        }

        // Non-null only for the private, stack-scoped lookup key used by
        // UrlStatSnapshot::add(). Keys inserted into UrlStatMap always own
        // url_ and leave this null.
        const View* view_{nullptr};
    };

    struct UrlKeyHash {
        size_t operator()(const UrlKey& key) const noexcept {
            const auto url_hash = key.url_hash();
            const auto tick_hash = std::hash<int64_t>{}(key.tick_);
            return url_hash ^ (tick_hash + 0x9e3779b97f4a7c15ULL + (url_hash << 6) + (url_hash >> 2));
        }
    };

    /**
     * @brief Raw runtime information for a single URL invocation.
     */
    struct UrlStatEntry {
        std::string url_pattern_;
        std::string method_;
        int status_code_;
        bool failed_;
        std::chrono::system_clock::time_point end_time_;
        int32_t elapsed_;

        UrlStatEntry(std::string_view url_pattern, std::string_view method, int status_code)
                : url_pattern_{url_pattern}, method_{method}, status_code_{status_code},
                  failed_{false}, end_time_{}, elapsed_{} {}
    };

    /**
     * @brief Snapshot of URL statistics aggregated over a time window.
     */
    class UrlStatSnapshot {
    public:
        using UrlStatMap = std::unordered_map<UrlKey, EachUrlStat, UrlKeyHash>;

        UrlStatSnapshot() : urlMap_{} {}
        ~UrlStatSnapshot() = default;
        UrlStatSnapshot(const UrlStatSnapshot&) = delete;
        UrlStatSnapshot& operator=(const UrlStatSnapshot&) = delete;
        
        /**
         * @brief Adds a URL statistic to the snapshot using bucketization rules.
         *
         * @param us URL statistic entry.
         * @param config Agent configuration for histogram settings.
         * @param tick_clock Clock for time bucketing.
         */
        void add(const UrlStatEntry* us, const Config& config, TickClock& tick_clock);
        /// @brief Returns the const map storing statistics per URL/tick.
        const UrlStatMap& getEachStats() const { return urlMap_; }

        /**
         * @brief Trims a URL path using the configured depth.
         *
         * @param url Raw URL path.
         * @param depth Maximum number of segments to keep.
         */
        static std::string trim_url_path(std::string_view url, int depth);

    private:
        UrlStatMap urlMap_;
    };

    /**
     * @brief Background workers for collecting and sending URL statistics.
     */
    class UrlStats {
    public:
        /**
         * @brief Constructs the URL statistics aggregator.
         *
         * @param agent Owning agent service.
         * @param tick_interval Time-bucket width for aggregation; defaults to
         *        the production value, tests inject shorter buckets.
         * @param send_interval Period of the send worker; defaults to the
         *        production value, tests inject millisecond periods.
         */
        explicit UrlStats(AgentService* agent,
                          std::chrono::seconds tick_interval = URL_STAT_TICK_INTERVAL,
                          std::chrono::milliseconds send_interval = URL_STAT_SEND_INTERVAL);
        ~UrlStats() = default;

        /**
         * @brief Queues a URL statistic for aggregation.
         *
         * @param stats URL statistic entry (ownership transferred).
         */
        void enqueueUrlStats(std::unique_ptr<UrlStatEntry> stats) noexcept;
        void enqueueUrlStats(UrlStatEntry stats) noexcept;
        /**
         * @brief Queues a URL statistic using the caller's config snapshot.
         *
         * Primary overload: the ones above load the current config and
         * delegate here. Hot-path callers (spans, via
         * AgentService::recordUrlStat) pass the snapshot they already hold,
         * skipping the atomic config load per record. `config` only has to
         * stay alive for the duration of the call.
         */
        void enqueueUrlStats(UrlStatEntry stats, const Config& config) noexcept;
        /// @brief Worker loop that aggregates URL statistics.
        void addUrlStatsWorker();
        /// @brief Stops the aggregation worker.
        void stopAddUrlStatsWorker();
        /// @brief Worker loop that sends aggregated statistics to the collector.
        void sendUrlStatsWorker();
        /// @brief Stops the sending worker.
        void stopSendUrlStatsWorker();

        /// @brief Adds a runtime statistic to the current snapshot buffer.
        void addSnapshot(const UrlStatEntry* us, const Config& config);
        /// @brief Extracts the latest URL statistic snapshot for transmission.
        std::unique_ptr<UrlStatSnapshot> takeSnapshot();
        /// @brief Returns the tick clock for time bucketing.
        TickClock& getTickClock() { return tick_clock_; }

    private:
        static constexpr size_t kQueueShardCount = 16;

        // One queue per shard: each request thread sticks to one shard
        // (picked by thread id), so enqueues from different threads mostly
        // take different mutexes instead of contending on one global lock.
        struct QueueShard {
            std::mutex mutex_;
            std::queue<UrlStatEntry> queue_;
        };

        QueueShard& queueShard();
        void drainQueueShards(const Config& config);
        /// @brief One supervised run of the aggregation loop; the public
        /// worker restarts it after a transient exception.
        void runAddUrlStatsWorker(const Config& config);
        /// @brief One supervised run of the periodic send loop.
        void runSendUrlStatsWorker();

        // Non-owning. AgentImpl owns this object (unique_ptr member) and joins
        // the URL-stat workers before its own destruction, so agent_ never
        // dangles. A shared_ptr here would form a cycle and leak the agent.
        AgentService* agent_{};

        // Queue for incoming URL stats. pending_ is the exact number of
        // queued-but-not-drained entries across all shards: it is incremented
        // under the same shard lock as the push and decremented under the same
        // shard lock as the drain swap, so it can never go negative and serves
        // both as the global size limit and the worker's wait predicate.
        // add_mutex_ only pairs enqueue wakeups with the worker's wait — it is
        // taken on the enqueue path solely on the empty→non-empty transition.
        std::array<QueueShard, kQueueShardCount> queue_shards_{};
        std::atomic<int64_t> pending_{0};
        std::mutex add_mutex_{};
        std::condition_variable add_cond_var_{};
        // Rate-limited overflow reporting (see QueueDropReporter).
        QueueDropReporter drop_reporter_{};

        // Snapshot management
        TickClock tick_clock_;
        std::unique_ptr<UrlStatSnapshot> snapshot_;
        std::mutex snapshot_mutex_{};

        // Send worker synchronization
        std::chrono::milliseconds send_interval_;
        std::mutex send_mutex_{};
        std::condition_variable send_cond_var_{};
    };
}
