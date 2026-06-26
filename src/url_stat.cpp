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

#include "logging.h"
#include "url_stat.h"

namespace pinpoint {

    // URL stat configuration constants
    constexpr int URL_STAT_TICK_INTERVAL_SECONDS = 30;
    constexpr int URL_STAT_SEND_INTERVAL_SECONDS = 30;
    
    // Histogram bucket thresholds (in milliseconds)
    constexpr int32_t BUCKET_THRESHOLD_100MS = 100;
    constexpr int32_t BUCKET_THRESHOLD_300MS = 300;
    constexpr int32_t BUCKET_THRESHOLD_500MS = 500;
    constexpr int32_t BUCKET_THRESHOLD_1S = 1000;
    constexpr int32_t BUCKET_THRESHOLD_3S = 3000;
    constexpr int32_t BUCKET_THRESHOLD_5S = 5000;
    constexpr int32_t BUCKET_THRESHOLD_8S = 8000;

    struct TrimmedUrlPath {
        std::string_view path;
        bool wildcard;
    };

    static TrimmedUrlPath trim_url_path_view(std::string_view url, int depth) noexcept {
        if (url.empty()) {
            return {url, false};
        }

        if (depth < 1) {
            depth = 1;
        }

        size_t end = url.size();
        bool wildcard = false;
        for (size_t i = 1; i < url.size(); i++) {
            if (url[i] == '?') {
                end = i;
                break;
            }
            if (url[i] == '/') {
                depth--;
                if (depth == 0) {
                    end = i + 1;
                    wildcard = true;
                    break;
                }
            }
        }

        return {url.substr(0, end), wildcard};
    }

    static std::string build_url_stat_key(const UrlStatEntry& us, const Config& config) {
        const auto trimmed = config.http.url_stat.enable_trim_path
            ? trim_url_path_view(us.url_pattern_, config.http.url_stat.trim_path_depth)
            : TrimmedUrlPath{us.url_pattern_, false};

        const auto method_prefix_size = config.http.url_stat.method_prefix ? us.method_.size() + 1 : 0;
        std::string url;
        url.reserve(method_prefix_size + trimmed.path.size() + (trimmed.wildcard ? 1 : 0));
        if (config.http.url_stat.method_prefix) {
            url.append(us.method_);
            url.push_back(' ');
        }
        url.append(trimmed.path.data(), trimmed.path.size());
        if (trimmed.wildcard) {
            url.push_back('*');
        }
        return url;
    }
    
    UrlStats::UrlStats(AgentService* agent)
        : agent_(agent),
          tick_clock_(URL_STAT_TICK_INTERVAL_SECONDS),
          snapshot_(std::make_unique<UrlStatSnapshot>()) {}

    void UrlStats::addSnapshot(const UrlStatEntry* us, const Config& config) {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_->add(us, config, tick_clock_);
    }

    std::unique_ptr<UrlStatSnapshot> UrlStats::takeSnapshot() {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        auto old_snapshot = std::move(snapshot_);
        snapshot_ = std::make_unique<UrlStatSnapshot>();
        return old_snapshot;
    }

    int64_t TickClock::tick(const std::chrono::system_clock::time_point end_time) const {
        const auto end_millis = std::chrono::duration_cast<std::chrono::milliseconds>(end_time.time_since_epoch());
        const auto interval = std::chrono::milliseconds(interval_ * 1000);
        const auto cutoff = std::chrono::duration_cast<std::chrono::milliseconds>(end_millis % interval);
        return end_millis.count() - cutoff.count();
    }

    static constexpr int getBucket(int32_t elapsed) noexcept {
        if (elapsed < BUCKET_THRESHOLD_100MS) return 0;
        if (elapsed < BUCKET_THRESHOLD_300MS) return 1;
        if (elapsed < BUCKET_THRESHOLD_500MS) return 2;
        if (elapsed < BUCKET_THRESHOLD_1S) return 3;
        if (elapsed < BUCKET_THRESHOLD_3S) return 4;
        if (elapsed < BUCKET_THRESHOLD_5S) return 5;
        if (elapsed < BUCKET_THRESHOLD_8S) return 6;
        return 7;
    }

    void UrlStatHistogram::add(int32_t elapsed) {
        total_ += elapsed;
        if (max_ < elapsed) {
            max_ = elapsed;
        }
        histogram_[getBucket(elapsed)]++;
    }

    void UrlStatSnapshot::add(const UrlStatEntry* us, const Config& config, TickClock& tick_clock) {
        if (us == nullptr) {
            return;
        }

        const auto tick = tick_clock.tick(us->end_time_);
        auto key = UrlKey{build_url_stat_key(*us, config), tick};
        LOG_DEBUG("url stats snapshot add : {}, {}", key.url_, key.tick_);

        EachUrlStat *e;
        if (const auto f = urlMap_.find(key); f == urlMap_.end()) {
            if (urlMap_.size() >= static_cast<size_t>(config.http.url_stat.limit)) {
                return;
            }
            if (urlMap_.empty() && config.http.url_stat.limit > 0) {
                constexpr size_t kMaxInitialReserve = 4096;
                urlMap_.reserve(std::min(static_cast<size_t>(config.http.url_stat.limit), kMaxInitialReserve));
            }
            auto new_stat = std::make_unique<EachUrlStat>(key.tick_);
            e = new_stat.get();
            urlMap_.emplace(std::move(key), std::move(new_stat));
        } else {
            e = f->second.get();
        }

        e->getTotalHistogram().add(us->elapsed_);
        if (us->failed_) {
            e->getFailHistogram().add(us->elapsed_);
        }
    }

    std::string UrlStatSnapshot::trim_url_path(std::string_view url, int depth) {
        const auto trimmed = trim_url_path_view(url, depth);
        std::string result;
        result.reserve(trimmed.path.size() + (trimmed.wildcard ? 1 : 0));
        result.append(trimmed.path.data(), trimmed.path.size());
        if (trimmed.wildcard) {
            result.push_back('*');
        }
        return result;
    }

    void UrlStats::enqueueUrlStats(std::unique_ptr<UrlStatEntry> stats) noexcept try {
        if (!stats) {
            return;
        }
        enqueueUrlStats(std::move(*stats));
    } catch (const std::exception &e) {
        LOG_ERROR("failed to enqueue url stats: exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("failed to enqueue url stats: unknown exception");
    }

    void UrlStats::enqueueUrlStats(UrlStatEntry stats) noexcept try {
        const auto config = agent_->getConfig();
        if (!config->http.url_stat.enable) {
            return;
        }

        std::unique_lock<std::mutex> lock(add_mutex_);

        if (url_stats_.size() < config->span.queue_size) {
            url_stats_.push(std::move(stats));
            lock.unlock();
            add_cond_var_.notify_one();
        } else {
            LOG_DEBUG("drop url stats: overflow max queue size {}", config->span.queue_size);
        }
    } catch (const std::exception &e) {
        LOG_ERROR("failed to enqueue url stats: exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("failed to enqueue url stats: unknown exception");
    }

    void UrlStats::addUrlStatsWorker() try {
        const auto config = agent_->getConfig();
        if (!config->http.url_stat.enable) {
            return;
        }

        std::unique_lock<std::mutex> lock(add_mutex_);
        std::queue<UrlStatEntry> batch;
        while (!agent_->isExiting()) {
            add_cond_var_.wait(lock, [this] {
                return !url_stats_.empty() || agent_->isExiting();
            });
            if (agent_->isExiting()) {
                break;
            }

            batch.swap(url_stats_);
            lock.unlock();
            {
                std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
                while (!batch.empty()) {
                    auto us = std::move(batch.front());
                    batch.pop();
                    snapshot_->add(&us, *config, tick_clock_);
                }
            }
            lock.lock();
        }
        LOG_INFO("add url stats worker end");
    } catch (const std::exception& e) {
        LOG_ERROR("add url stats worker exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("add url stats worker unknown exception");
    }

    void UrlStats::stopAddUrlStatsWorker() {
        const auto config = agent_->getConfig();
        if (!config->http.url_stat.enable) {
            return;
        }

        add_cond_var_.notify_one();
    }

    void UrlStats::sendUrlStatsWorker() try {
        const auto config = agent_->getConfig();
        if (!config->http.url_stat.enable) {
            return;
        }

        std::unique_lock<std::mutex> lock(send_mutex_);
        const auto timeout = std::chrono::seconds(URL_STAT_SEND_INTERVAL_SECONDS);

        while (!agent_->isExiting()) {
            if (!send_cond_var_.wait_for(lock, timeout, [this]{ return agent_->isExiting(); })) {
                agent_->recordStats(URL_STATS);
            }
        }

        LOG_INFO("send url stats worker end");
    } catch (const std::exception& e) {
        LOG_ERROR("send url stats worker exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("send url stats worker unknown exception");
    }

    void UrlStats::stopSendUrlStatsWorker() {
        const auto config = agent_->getConfig();
        if (!config->http.url_stat.enable) {
            return;
        }

        send_cond_var_.notify_one();
    }
}
