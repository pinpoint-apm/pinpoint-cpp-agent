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
#include <thread>

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

    UrlStats::QueueShard& UrlStats::queueShard() {
        // Hashed once per thread: each application thread sticks to one shard
        // for its lifetime, so concurrent enqueues spread across shard mutexes
        // instead of serializing on a single queue lock.
        static const thread_local size_t shard_index =
            std::hash<std::thread::id>{}(std::this_thread::get_id()) % kQueueShardCount;
        return queue_shards_[shard_index];
    }

    void UrlStats::enqueueUrlStats(UrlStatEntry stats) noexcept try {
        const auto config = agent_->getConfig();
        if (!config->http.url_stat.enable) {
            return;
        }

        auto& shard = queueShard();
        int64_t prev_pending;
        {
            std::lock_guard<std::mutex> shard_lock(shard.mutex_);
            prev_pending = pending_.load(std::memory_order_relaxed);
            if (prev_pending >= static_cast<int64_t>(config->span.queue_size)) {
                LOG_DEBUG("drop url stats: overflow max queue size {}", config->span.queue_size);
                return;
            }
            shard.queue_.push(std::move(stats));
            pending_.fetch_add(1, std::memory_order_relaxed);
        }

        // Wake the worker only on the empty→non-empty transition; while
        // entries are already pending the worker is awake (or will re-check
        // its predicate before blocking), so the common case skips add_mutex_
        // entirely. Taking the (empty) lock pairs the notify with the worker's
        // predicate check, so it cannot fire between the worker reading
        // pending_ == 0 and blocking — a lost wakeup that would strand the
        // entry until the next enqueue.
        if (prev_pending == 0) {
            { std::lock_guard<std::mutex> lock(add_mutex_); }
            add_cond_var_.notify_one();
        }
    } catch (const std::exception &e) {
        LOG_ERROR("failed to enqueue url stats: exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("failed to enqueue url stats: unknown exception");
    }

    void UrlStats::drainQueueShards(const Config& config) {
        // Bound how long snapshot_mutex_ is held per acquisition: a drained
        // batch can be as large as the whole queue limit, and each add does a
        // key build plus a map lookup — processing it under one lock would
        // stall takeSnapshot (the send path) for the entire batch.
        constexpr size_t kEntriesPerSnapshotLock = 64;

        for (auto& shard : queue_shards_) {
            std::queue<UrlStatEntry> batch;
            {
                std::lock_guard<std::mutex> shard_lock(shard.mutex_);
                batch.swap(shard.queue_);
                if (!batch.empty()) {
                    pending_.fetch_sub(static_cast<int64_t>(batch.size()), std::memory_order_relaxed);
                }
            }

            while (!batch.empty()) {
                std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
                for (size_t i = 0; i < kEntriesPerSnapshotLock && !batch.empty(); i++) {
                    auto us = std::move(batch.front());
                    batch.pop();
                    snapshot_->add(&us, config, tick_clock_);
                }
            }
        }
    }

    void UrlStats::addUrlStatsWorker() try {
        const auto config = agent_->getConfig();
        if (!config->http.url_stat.enable) {
            return;
        }

        std::unique_lock<std::mutex> lock(add_mutex_);
        while (!agent_->isExiting()) {
            // Entries enqueued while draining don't re-notify (only the
            // empty→non-empty transition does), so the predicate re-check on
            // pending_ before blocking is what picks them up.
            add_cond_var_.wait(lock, [this] {
                return pending_.load(std::memory_order_relaxed) > 0 || agent_->isExiting();
            });
            if (agent_->isExiting()) {
                break;
            }

            lock.unlock();
            drainQueueShards(*config);
            lock.lock();
        }
        LOG_INFO("add url stats worker end");
    } catch (const std::exception& e) {
        LOG_ERROR("add url stats worker exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("add url stats worker unknown exception");
    }

    void UrlStats::stopAddUrlStatsWorker() {
        // Always notify, regardless of the CURRENT config: the worker decided
        // whether to run from the config it saw at startup, and CollectUrlStat
        // is reloadable — consulting the live config here could skip the only
        // wakeup for a worker parked in an untimed wait and hang shutdown.
        // Taking the lock pairs the notify with the worker's predicate check
        // (isExiting), so it cannot fire in the window between the worker
        // reading the flag as false and blocking — a lost wakeup that would
        // also hang shutdown forever.
        std::lock_guard<std::mutex> lock(add_mutex_);
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
        // Same rationale as stopAddUrlStatsWorker: notify unconditionally and
        // under the lock. This worker's wait_for bounds a lost wakeup at 30s
        // rather than forever, but shutdown should not stall at all.
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_cond_var_.notify_one();
    }
}
