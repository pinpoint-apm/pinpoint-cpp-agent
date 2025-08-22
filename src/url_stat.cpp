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

#include <mutex>
#include <sstream>

#include "absl/strings/str_cat.h"
#include "agent.h"
#include "logging.h"
#include "grpc.h"
#include "url_stat.h"

namespace pinpoint {

    static TickClock tick_clock(30);
    static std::mutex snapshot_mutex;
    static UrlStatSnapshot *url_stats_snapshot;

    void init_url_stat() {
        std::unique_lock<std::mutex> lock(snapshot_mutex);
        url_stats_snapshot = new UrlStatSnapshot();
    }

    void add_url_stat_snapshot(const UrlStat* us, const Config& config) {
        std::unique_lock<std::mutex> lock(snapshot_mutex);
        url_stats_snapshot->add(us, config);
    }

    UrlStatSnapshot* take_url_stat_snapshot() {
        std::unique_lock<std::mutex> lock(snapshot_mutex);
        const auto snapshot = url_stats_snapshot;
        url_stats_snapshot = new UrlStatSnapshot();
        return snapshot;
    }

    int64_t TickClock::tick(const std::chrono::system_clock::time_point end_time) const {
        const auto end_millis = std::chrono::duration_cast<std::chrono::milliseconds>(end_time.time_since_epoch());
        const auto interval = std::chrono::milliseconds(interval_ * 1000);
        const auto cutoff = std::chrono::duration_cast<std::chrono::milliseconds>(end_millis % interval);
        return end_millis.count() - cutoff.count();
    }

    static int getBucket(int32_t elapsed) {
        if (elapsed < 100) {
            return 0;
        } else if (elapsed < 300) {
            return 1;
        } else if (elapsed < 500) {
            return 2;
        } else if (elapsed < 1000) {
            return 3;
        } else if (elapsed < 3000) {
            return 4;
        } else if (elapsed < 5000) {
            return 5;
        } else if (elapsed < 8000) {
            return 6;
        } else {
            return 7;
        }
    }

    void UrlStatHistogram::add(int32_t elapsed) {
        total_ += elapsed;
        if (max_ < elapsed) {
            max_ = elapsed;
        }
        histogram_[getBucket(elapsed)]++;
    }

    static int url_status(int status)  {
        if (status/100 < 4) {
            return URL_STATUS_SUCCESS;
        } else {
            return URL_STATUS_FAIL;
        }
    }

    void UrlStatSnapshot::add(const UrlStat* us, const Config& config) {
        std::unique_lock<std::mutex> lock(mutex_);

        auto url = trim_url_path(us->url_pattern_, config.http.url_stat.path_depth);

        if (config.http.url_stat.method_prefix) {
            url = absl::StrCat(us->method_, " ", url);
        }

        const auto key = UrlKey{url, tick_clock.tick(us->end_time_)};
        LOG_DEBUG("url stats snapshot add : {}, {}", key.url_, key.tick_);

        EachUrlStat *e;
        if (const auto f = urlMap_.find(key); f == urlMap_.end()) {
            if (count_ >= config.http.url_stat.limit) {
                return;
            }
            e = new EachUrlStat(url, key.tick_);
            urlMap_[key] =  e;
            count_++;
        } else {
            e = f->second;
        }

        e->getTotalHistogram().add(us->elapsed_);
        if (url_status(us->status_code_) == URL_STATUS_FAIL) {
            e->getFailHistogram().add(us->elapsed_);
        }
    }

    std::string trim_url_path(std::string_view url, int depth) {
        std::stringstream ss;
        auto len = url.size();
        bool tailing = false;

        if (depth < 1) { depth = 1; }

        ss << url[0];
        for (size_t i = 1; i < len; i++) {
            if (url[i] == '?') {
                break;
            }

            ss << url[i];
            if (url[i] == '/') {
                depth--;
                if (depth == 0) {
                    tailing = true;
                    break;
                }
            }
        }

        if (tailing) {
            ss << '*';
        }
        return ss.str();
    }

    void UrlStats::enqueueUrlStats(std::unique_ptr<UrlStat> stats) noexcept try {
        if (auto& config = agent_->getConfig(); !config.http.url_stat.enable) {
            return;
        }

        std::unique_lock<std::mutex> lock(add_mutex_);

        if (auto& config = agent_->getConfig(); url_stats_.size() < config.span.queue_size) {
            url_stats_.push(std::move(stats));
        } else {
            LOG_DEBUG("drop url stats: overflow max queue size {}", config.span.queue_size);
        }

        add_cond_var_.notify_one();
    } catch (const std::exception &e) {
        LOG_ERROR("failed to enqueue url stats: exception = {}", e.what());
    }

    void UrlStats::addUrlStatsWorker() try {
        auto& config = agent_->getConfig();
        if (!config.http.url_stat.enable) {
            return;
        }

        std::unique_lock<std::mutex> lock(add_mutex_);
        while (!agent_->isExiting()) {
            add_cond_var_.wait(lock, [this] {
                return !url_stats_.empty() || agent_->isExiting();
            });
            if (agent_->isExiting()) {
                break;
            }

            auto us = std::move(url_stats_.front());
            url_stats_.pop();
            lock.unlock();
            add_url_stat_snapshot(us.get(), config);
            lock.lock();
        }
        LOG_INFO("add url stats worker end");
    } catch (const std::exception& e) {
        LOG_ERROR("add url stats worker exception = {}", e.what());
    }

    void UrlStats::stopAddUrlStatsWorker() {
        if (auto& config = agent_->getConfig(); !config.http.url_stat.enable) {
            return;
        }

        std::unique_lock<std::mutex> lock(add_mutex_);
        add_cond_var_.notify_one();
    }

    void UrlStats::sendUrlStatsWorker() try {
        if (auto& config = agent_->getConfig(); !config.http.url_stat.enable) {
            return;
        }

        std::unique_lock<std::mutex> lock(send_mutex_);
        constexpr auto timeout = std::chrono::seconds(30);

        init_agent_stats();

        while (!agent_->isExiting()) {
            if (!send_cond_var_.wait_for(lock, timeout, [this]{ return agent_->isExiting(); })) {
                agent_->recordStats(URL_STATS);
            }
        }

        LOG_INFO("send url stats worker end");
    } catch (const std::exception& e) {
        LOG_ERROR("send url stats worker exception = {}", e.what());
    }

    void UrlStats::stopSendUrlStatsWorker() {
        if (auto& config = agent_->getConfig(); !config.http.url_stat.enable) {
            return;
        }

        std::unique_lock<std::mutex> lock(send_mutex_);
        send_cond_var_.notify_one();
    }
}
