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

#include "absl/strings/str_cat.h"
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
    
    // HTTP status code threshold
    constexpr int HTTP_STATUS_ERROR_THRESHOLD = 400;

    /**
     * @brief Singleton class to manage global URL statistics snapshot state.
     * 
     * This class encapsulates the global URL statistics snapshot and provides
     * thread-safe access to it. It replaces the previous global static variables
     * with a cleaner, RAII-compliant design.
     */
    class UrlStatSnapshotManager {
    public:
        static UrlStatSnapshotManager& getInstance() {
            static UrlStatSnapshotManager instance;
            return instance;
        }

        // Delete copy and move constructors
        UrlStatSnapshotManager(const UrlStatSnapshotManager&) = delete;
        UrlStatSnapshotManager& operator=(const UrlStatSnapshotManager&) = delete;
        UrlStatSnapshotManager(UrlStatSnapshotManager&&) = delete;
        UrlStatSnapshotManager& operator=(UrlStatSnapshotManager&&) = delete;

        void initialize() {
            std::unique_lock<std::mutex> lock(mutex_);
            snapshot_ = std::make_unique<UrlStatSnapshot>();
        }

        void add(const UrlStat* us, const Config& config) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (snapshot_) {
                snapshot_->add(us, config);
            }
        }

        std::unique_ptr<UrlStatSnapshot> takeSnapshot() {
            std::unique_lock<std::mutex> lock(mutex_);
            auto old_snapshot = std::move(snapshot_);
            snapshot_ = std::make_unique<UrlStatSnapshot>();
            return old_snapshot;
        }

        TickClock& getTickClock() {
            return tick_clock_;
        }

    private:
        UrlStatSnapshotManager() 
            : tick_clock_(URL_STAT_TICK_INTERVAL_SECONDS),
              snapshot_(nullptr) {}

        ~UrlStatSnapshotManager() = default;

        TickClock tick_clock_;
        std::unique_ptr<UrlStatSnapshot> snapshot_;
        std::mutex mutex_;
    };

    void init_url_stat() {
        UrlStatSnapshotManager::getInstance().initialize();
    }

    void add_url_stat_snapshot(const UrlStat* us, const Config& config) {
        UrlStatSnapshotManager::getInstance().add(us, config);
    }

    std::unique_ptr<UrlStatSnapshot> take_url_stat_snapshot() {
        return UrlStatSnapshotManager::getInstance().takeSnapshot();
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

    static constexpr int url_status(int status) noexcept {
        return status < HTTP_STATUS_ERROR_THRESHOLD ? URL_STATUS_SUCCESS : URL_STATUS_FAIL;
    }

    void UrlStatSnapshot::add(const UrlStat* us, const Config& config) {
        std::unique_lock<std::mutex> lock(mutex_);

        auto url = trim_url_path(us->url_pattern_, config.http.url_stat.path_depth);

        if (config.http.url_stat.method_prefix) {
            url = absl::StrCat(us->method_, " ", url);
        }

        auto& tick_clock = UrlStatSnapshotManager::getInstance().getTickClock();
        const auto key = UrlKey{url, tick_clock.tick(us->end_time_)};
        LOG_DEBUG("url stats snapshot add : {}, {}", key.url_, key.tick_);

        EachUrlStat *e;
        if (const auto f = urlMap_.find(key); f == urlMap_.end()) {
            if (urlMap_.size() >= static_cast<size_t>(config.http.url_stat.limit)) {
                return;
            }
            auto new_stat = std::make_unique<EachUrlStat>(key.tick_);
            e = new_stat.get();
            urlMap_[key] = std::move(new_stat);
        } else {
            e = f->second.get();
        }

        e->getTotalHistogram().add(us->elapsed_);
        if (url_status(us->status_code_) == URL_STATUS_FAIL) {
            e->getFailHistogram().add(us->elapsed_);
        }
    }

    std::string trim_url_path(std::string_view url, int depth) {
        if (url.empty()) {
            return "";
        }
        
        if (depth < 1) { 
            depth = 1; 
        }

        std::string result;
        result.reserve(url.size());  // Pre-allocate to avoid reallocation
        result += url[0];
        
        bool tailing = false;
        for (size_t i = 1; i < url.size(); i++) {
            if (url[i] == '?') {
                break;
            }
            result += url[i];
            if (url[i] == '/') {
                depth--;
                if (depth == 0) {
                    tailing = true;
                    break;
                }
            }
        }
        
        if (tailing) {
            result += '*';
        }
        return result;
    }

    void UrlStats::enqueueUrlStats(std::unique_ptr<UrlStat> stats) noexcept try {
        const auto& config = agent_->getConfig();
        if (!config.http.url_stat.enable) {
            return;
        }

        std::unique_lock<std::mutex> lock(add_mutex_);

        if (url_stats_.size() < config.span.queue_size) {
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
        const auto timeout = std::chrono::seconds(URL_STAT_SEND_INTERVAL_SECONDS);

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
