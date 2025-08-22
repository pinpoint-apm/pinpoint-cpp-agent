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

namespace pinpoint {
    constexpr int URL_STATS_BUCKET_SIZE      = 8;
    constexpr int URL_STATS_BUCKET_VERSION   = 0;
    constexpr int URL_STATUS_SUCCESS         = 1;
    constexpr int URL_STATUS_FAIL            = 2;

    class TickClock {
    public:
        explicit TickClock(int64_t interval) : interval_(interval) {}
        int64_t tick(std::chrono::system_clock::time_point end_time) const;

    private:
        int64_t interval_;
    };

    class UrlStatHistogram {
    public:
        UrlStatHistogram() : total_(0), max_(0) {
            std::fill_n(histogram_, URL_STATS_BUCKET_SIZE, 0);
        }
        ~UrlStatHistogram() = default;

        void add(int32_t elapsed);
        int64_t total() const { return total_; }
        int64_t max() const { return max_; }
        int32_t histogram(int index) const { return histogram_[index]; }

    private:
        int64_t total_;
        int64_t max_;
        int32_t histogram_[URL_STATS_BUCKET_SIZE]{};
    };


    class EachUrlStat {
    public:
        EachUrlStat(std::string url, int64_t tick) : url_(std::move(url)), tickTime_(tick) {}
        ~EachUrlStat() = default;

        UrlStatHistogram& getTotalHistogram() { return totalHistogram_; }
        UrlStatHistogram& getFailHistogram() { return failedHistogram_; }
        int64_t tick() const { return tickTime_; }

    private:
        std::string url_;
        UrlStatHistogram totalHistogram_;
        UrlStatHistogram failedHistogram_;
        int64_t tickTime_;
    };

    struct UrlKey  {
        std::string url_;
        int64_t tick_;
        bool operator<(const UrlKey &o)  const {
            return url_ < o.url_ || (url_ == o.url_ && tick_ < o.tick_);
        }
    };

    struct UrlStat  {
        std::string url_pattern_;
        std::string method_;
        int status_code_;
        std::chrono::system_clock::time_point end_time_;
        int32_t elapsed_;

        UrlStat(std::string_view url_pattern, std::string_view method, int status_code)
                : url_pattern_{url_pattern}, method_{method}, status_code_{status_code},
                  end_time_{}, elapsed_{} {}
    };

    class UrlStatSnapshot {
    public:
        UrlStatSnapshot() :count_(0), urlMap_{}, mutex_() {}
        ~UrlStatSnapshot() {
            for(auto& iter : urlMap_) {
                delete iter.second;
            }
            urlMap_.clear();
        }
        void add(const UrlStat* us, const Config& config);
        std::map<UrlKey, EachUrlStat*>& getEachStats() { return urlMap_; }

    private:
        int count_;
        std::map<UrlKey, EachUrlStat*> urlMap_;
        std::mutex mutex_;
    };

    class AgentImpl;

    class UrlStats {
    public:
        explicit UrlStats(AgentImpl* agent) : agent_(agent) {}

        void enqueueUrlStats(std::unique_ptr<UrlStat> stats) noexcept;
        void addUrlStatsWorker();
        void stopAddUrlStatsWorker();
        void sendUrlStatsWorker();
        void stopSendUrlStatsWorker();

    private:
        AgentImpl* agent_{};
        std::mutex add_mutex_{};
        std::condition_variable add_cond_var_{};
        std::queue<std::unique_ptr<UrlStat>> url_stats_{};

        std::mutex send_mutex_{};
        std::condition_variable send_cond_var_{};
    };

    void init_url_stat();
    void add_url_stat_snapshot(const UrlStat* us, const Config& config);
    UrlStatSnapshot* take_url_stat_snapshot();
    std::string trim_url_path(std::string_view url, int depth);
}
