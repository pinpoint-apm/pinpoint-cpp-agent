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

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
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
    // Stand-in key for a span that recorded URL stats without a URL, so the
    // server shows an identifiable bucket instead of an empty string. The
    // value is Java's URITemplate.NULL_URI verbatim, so a mixed Java/C++
    // application aggregates its "no URI recorded" traffic under one key
    // instead of two; the Go agent's own stand-in ("UNKNOWN_URL") differs.
    constexpr std::string_view URL_STAT_UNKNOWN = "/NULL";
    constexpr int URL_STATS_BUCKET_VERSION   = 0;

    // Production intervals; UrlStats accepts overrides so tests can drive the
    // tick bucketing and the periodic send in milliseconds instead of 30s.
    constexpr auto URL_STAT_TICK_INTERVAL = std::chrono::seconds(30);
    constexpr auto URL_STAT_SEND_INTERVAL = std::chrono::seconds(30);

    /// @brief Fixed-interval clock used to bucketize URL statistics.
    class TickClock {
    public:
        // Clamped to >= 1: tick() computes `end_millis % interval`, so a
        // non-positive interval would be a division by zero (SIGFPE).
        // Production always passes the URL_STAT_TICK_INTERVAL constant; the
        // clamp guards the test-injection path (see UrlStats' ctor).
        explicit TickClock(int64_t interval) : interval_(interval > 0 ? interval : 1) {}
        /// @brief Returns the tick for a span's completion time.
        int64_t tick(std::chrono::system_clock::time_point end_time) const;

    private:
        int64_t interval_;
    };

    /// @brief Histogram aggregating elapsed times for URL statistics.
    class UrlStatHistogram {
    public:
        UrlStatHistogram() = default;
        ~UrlStatHistogram() = default;

        /// @brief Adds an elapsed-time sample (milliseconds).
        void add(int32_t elapsed);
        /// @brief Folds another histogram of the same key into this one.
        void merge(const UrlStatHistogram& other);
        int64_t total() const { return total_; }
        int64_t max() const { return max_; }
        int32_t histogram(int index) const {
            if (index < 0 || index >= URL_STATS_BUCKET_SIZE) {
                return 0;
            }
            return histogram_[index]; 
        }
        /// @brief True when no sample has been recorded, so the histogram can
        /// travel as an empty message (see build_url_histogram).
        ///
        /// Every bucket zero, which is Go's urlStatHistogram.isEmpty()
        /// (url_stat.go:153-160) verbatim. Java asks getCount() == 0
        /// (UriStatMapper.java:63-69); there is no count member here, and
        /// total_ cannot stand in for one because it sums elapsed times and
        /// stays 0 for a histogram of nothing but 0ms samples. add() bumps
        /// exactly one bucket per sample, so the bucket sum is the count.
        bool empty() const {
            for (const auto count : histogram_) {
                if (count != 0) {
                    return false;
                }
            }
            return true;
        }

    private:
        int64_t total_{0};
        int64_t max_{0};
        int32_t histogram_[URL_STATS_BUCKET_SIZE]{};
    };


    /// @brief Statistics tracked for a single URL pattern and tick. The tick
    /// itself lives in the owning map's UrlKey.
    struct EachUrlStat {
        UrlStatHistogram total;
        UrlStatHistogram fail;
    };

    /// @brief Key identifying URL statistics by pattern and tick.
    struct UrlKey {
        std::string url_;
        int64_t tick_;

        bool operator==(const UrlKey& other) const noexcept {
            return tick_ == other.tick_ && url_ == other.url_;
        }
    };

    struct UrlKeyHash {
        size_t operator()(const UrlKey& key) const noexcept {
            const auto url_hash = std::hash<std::string>{}(key.url_);
            const auto tick_hash = std::hash<int64_t>{}(key.tick_);
            return url_hash ^ (tick_hash + 0x9e3779b97f4a7c15ULL + (url_hash << 6) + (url_hash >> 2));
        }
    };

    /// @brief Raw runtime information for a single URL invocation.
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

    /// @brief Snapshot of URL statistics aggregated over a time window.
    class UrlStatSnapshot {
    public:
        using UrlStatMap = std::unordered_map<UrlKey, EachUrlStat, UrlKeyHash>;

        UrlStatSnapshot() : urlMap_{} {}
        ~UrlStatSnapshot() = default;
        UrlStatSnapshot(const UrlStatSnapshot&) = delete;
        UrlStatSnapshot& operator=(const UrlStatSnapshot&) = delete;
        
        /// @brief Adds a URL statistic using the bucketization rules.
        /// @return false when url_stat.limit rejected a new key; true otherwise.
        bool add(const UrlStatEntry* us, const Config& config, TickClock& tick_clock);
        /// @brief Moves @p other's entries into this snapshot, folding the
        /// histograms of any key present in both. Deliberately does not
        /// re-check url_stat.limit: the limit caps each tick as it is
        /// collected, and merging is what assembles those ticks for one send.
        void merge(UrlStatSnapshot& other);
        const UrlStatMap& getEachStats() const { return urlMap_; }
        bool empty() const { return urlMap_.empty(); }
        /// @brief Newest tick added so far; 0 while empty.
        int64_t tick() const { return tick_; }

        /// @brief Trims a URL path to at most @p depth segments. Production
        /// uses the string_view form internally; this is the checkable seam.
        static std::string trim_url_path(std::string_view url, int depth);

    private:
        UrlStatMap urlMap_;
        int64_t tick_{0};
    };

    /// @brief Background workers for collecting and sending URL statistics.
    class UrlStats {
    public:
        /// @brief @p tick_interval (bucket width) and @p send_interval default
        ///        to the production values; tests inject shorter ones.
        explicit UrlStats(AgentService* agent,
                          std::chrono::seconds tick_interval = URL_STAT_TICK_INTERVAL,
                          std::chrono::milliseconds send_interval = URL_STAT_SEND_INTERVAL);
        ~UrlStats() = default;

        /// @brief Queues a URL statistic for aggregation.
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
        void stopAddUrlStatsWorker();
        /// @brief Worker loop that sends aggregated statistics to the collector.
        void sendUrlStatsWorker();
        void stopSendUrlStatsWorker();

        /// @brief Adds a runtime statistic to the current snapshot buffer.
        void addSnapshot(const UrlStatEntry* us, const Config& config);
        /// @brief Extracts the ticks already cut into completed_, merged
        /// into a single snapshot. Empty when nothing was cut since the last
        /// call — the caller must then send no message at all, the way Java's
        /// UriStatCollectingJob stops on a null poll
        /// (UriStatCollectingJob.java:52-55).
        /// @param include_in_progress also takes the tick still being
        /// collected, splitting it across two messages. Only the shutdown
        /// flush passes true: nothing will arrive later to cut that tick, so
        /// there it is split-or-lose rather than split-or-wait.
        std::unique_ptr<UrlStatSnapshot> takeSnapshot(bool include_in_progress = false);

    private:
        static constexpr size_t kQueueShardCount = 16;
        // Completed ticks retained while the stats stream is down, matching
        // Java's AsyncQueueingUriStatStorage snapshotQueue capacity of 4 —
        // two minutes of 30s ticks. Bounded because a stream that never
        // recovers would otherwise grow this without limit; the oldest tick
        // is the one worth losing first.
        static constexpr size_t kMaxCompletedSnapshots = 4;

        // One queue per shard: each request thread sticks to one shard
        // (picked by thread id), so enqueues from different threads mostly
        // take different mutexes instead of contending on one global lock.
        struct QueueShard {
            std::mutex mutex_;
            std::queue<UrlStatEntry> queue_;
        };

        QueueShard& queueShard();
        void drainQueueShards(const Config& config);
        /// @brief Cuts the snapshot at a tick boundary, then aggregates @p us.
        ///        Caller must hold snapshot_mutex_.
        void addLocked(const UrlStatEntry& us, const Config& config);
        /// @brief One supervised run of the aggregation loop; the public
        /// worker restarts it after a transient exception.
        void runAddUrlStatsWorker(const Config& config);
        /// @brief One supervised run of the periodic send loop.
        void runSendUrlStatsWorker();

        // Non-owning. The agent joins the URL-stat workers before its own
        // destruction, and this object can now outlive the agent (shared
        // with every AgentRuntime snapshot) — but the only method that runs
        // in that afterlife is the config-taking enqueueUrlStats overload,
        // which never reads agent_ (and drops on accepting_ anyway). The
        // config-loading overload and the workers, which do read it, run
        // only while the agent is alive. A shared_ptr here would form a
        // cycle and leak the agent.
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
        // Flipped once by stopAddUrlStatsWorker(); enqueueUrlStats drops
        // entries from then on. Spans that reach this sink through their
        // runtime snapshot (UnsampledSpan) bypass the agent's enabled_ check
        // in recordUrlStat, and the snapshot keeps this object alive past
        // agent teardown — this flag preserves the old behavior of dropping
        // post-shutdown entries instead of buffering them into queues no
        // worker will ever drain.
        std::atomic<bool> accepting_{true};
        // Rate-limited overflow reporting (see QueueDropReporter). One per
        // drop cause: sharing a reporter would conflate the counts and let
        // whichever fires first silence the others for a whole window.
        QueueDropReporter queue_drop_reporter_{};
        QueueDropReporter limit_drop_reporter_{};
        QueueDropReporter snapshot_drop_reporter_{};

        // Snapshot management
        TickClock tick_clock_;
        // snapshot_ collects the tick in progress; completed_ holds the ticks
        // already cut, oldest first, until a send drains them. Both are under
        // snapshot_mutex_. Keeping each tick separate is what makes
        // url_stat.limit a per-tick capacity: a stalled stream no longer packs
        // N ticks' keys into one limit-capped map.
        std::unique_ptr<UrlStatSnapshot> snapshot_;
        std::deque<std::unique_ptr<UrlStatSnapshot>> completed_{};
        std::mutex snapshot_mutex_{};

        // Send worker synchronization
        std::chrono::milliseconds send_interval_;
        std::mutex send_mutex_{};
        std::condition_variable send_cond_var_{};
    };
}
