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

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <string>

#include "config.h"
#include "agent_service.h"

namespace pinpoint {
    constexpr int URL_STATS_BUCKET_SIZE      = 8;
    constexpr int URL_STATS_BUCKET_VERSION   = 0;
    constexpr int URL_STATUS_SUCCESS         = 1;
    constexpr int URL_STATUS_FAIL            = 2;

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
        int32_t histogram(int index) const { return histogram_[index]; }

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
        EachUrlStat(std::string url, int64_t tick) : url_(std::move(url)), tickTime_(tick) {}
        ~EachUrlStat() = default;

        /// @brief Returns the histogram aggregating all responses.
        UrlStatHistogram& getTotalHistogram() { return totalHistogram_; }
        /// @brief Returns the histogram aggregating failed responses.
        UrlStatHistogram& getFailHistogram() { return failedHistogram_; }
        /// @brief Returns the tick value associated with this statistic.
        int64_t tick() const { return tickTime_; }

    private:
        std::string url_;
        UrlStatHistogram totalHistogram_;
        UrlStatHistogram failedHistogram_;
        int64_t tickTime_;
    };

    /**
     * @brief Key used to order URL statistics by pattern and tick.
     */
    struct UrlKey {
        std::string url_;
        int64_t tick_;
        bool operator<(const UrlKey &o)  const {
            return url_ < o.url_ || (url_ == o.url_ && tick_ < o.tick_);
        }
    };

    /**
     * @brief Raw runtime information for a single URL invocation.
     */
    struct UrlStat {
        std::string url_pattern_;
        std::string method_;
        int status_code_;
        std::chrono::system_clock::time_point end_time_;
        int32_t elapsed_;

        UrlStat(std::string_view url_pattern, std::string_view method, int status_code)
                : url_pattern_{url_pattern}, method_{method}, status_code_{status_code},
                  end_time_{}, elapsed_{} {}
    };

    /**
     * @brief Snapshot of URL statistics aggregated over a time window.
     */
    class UrlStatSnapshot {
    public:
        UrlStatSnapshot() :count_(0), urlMap_{}, mutex_() {}
        ~UrlStatSnapshot() {
            for(auto& iter : urlMap_) {
                delete iter.second;
            }
            urlMap_.clear();
        }
        /**
         * @brief Adds a URL statistic to the snapshot using bucketization rules.
         *
         * @param us URL statistic entry.
         * @param config Agent configuration for histogram settings.
         */
        void add(const UrlStat* us, const Config& config);
        /// @brief Returns the map storing statistics per URL/tick.
        std::map<UrlKey, EachUrlStat*>& getEachStats() { return urlMap_; }

    private:
        int count_;
        std::map<UrlKey, EachUrlStat*> urlMap_;
        std::mutex mutex_;
    };

    /**
     * @brief Background workers for collecting and sending URL statistics.
     */
    class UrlStats {
    public:
        explicit UrlStats(AgentService* agent) : agent_(agent) {}

        /**
         * @brief Queues a URL statistic for aggregation.
         *
         * @param stats URL statistic entry (ownership transferred).
         */
        void enqueueUrlStats(std::unique_ptr<UrlStat> stats) noexcept;
        /// @brief Worker loop that aggregates URL statistics.
        void addUrlStatsWorker();
        /// @brief Stops the aggregation worker.
        void stopAddUrlStatsWorker();
        /// @brief Worker loop that sends aggregated statistics to the collector.
        void sendUrlStatsWorker();
        /// @brief Stops the sending worker.
        void stopSendUrlStatsWorker();

    private:
        AgentService* agent_{};
        std::mutex add_mutex_{};
        std::condition_variable add_cond_var_{};
        std::queue<std::unique_ptr<UrlStat>> url_stats_{};

        std::mutex send_mutex_{};
        std::condition_variable send_cond_var_{};
    };

    /// @brief Initializes the global URL statistics buffers.
    void init_url_stat();
    /// @brief Adds a runtime statistic to the current snapshot buffer.
    void add_url_stat_snapshot(const UrlStat* us, const Config& config);
    /// @brief Extracts the latest URL statistic snapshot for transmission.
    UrlStatSnapshot* take_url_stat_snapshot();
    /**
     * @brief Trims a URL path using the configured depth.
     *
     * @param url Raw URL path.
     * @param depth Maximum number of segments to keep.
     */
    std::string trim_url_path(std::string_view url, int depth);
}
