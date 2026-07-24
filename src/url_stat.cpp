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
    
    UrlStats::UrlStats(AgentService* agent,
                       std::chrono::seconds tick_interval,
                       std::chrono::milliseconds send_interval)
        : agent_(agent),
          tick_clock_(tick_interval.count()),
          snapshot_(std::make_unique<UrlStatSnapshot>()),
          send_interval_(send_interval) {}

    void UrlStats::addSnapshot(const UrlStatEntry* us, const Config& config) {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_->add(us, config, tick_clock_);
    }

    std::unique_ptr<UrlStatSnapshot> UrlStats::takeSnapshot() {
        // Allocate the replacement before touching snapshot_: if this throws
        // under memory pressure the member must stay intact — moving it out
        // first would leave it null after the caller's catch, and the next
        // addSnapshot()/drainQueueShards() would dereference a null pointer.
        auto fresh = std::make_unique<UrlStatSnapshot>();
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.swap(fresh);
        return fresh;
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
        // Defensive clamp at the aggregation sink: the span paths already
        // clamp their system_clock-derived elapsed (setEndTime and
        // UnsampledSpan::EndSpan), but a negative value slipping in from any
        // other producer would silently decrement total_ and skew the
        // reported average — getBucket() alone would mask it into bucket 0.
        if (elapsed < 0) {
            elapsed = 0;
        }
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
        enqueueUrlStats(std::move(stats), *config);
    } catch (const std::exception &e) {
        LOG_ERROR("failed to enqueue url stats: exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("failed to enqueue url stats: unknown exception");
    }

    void UrlStats::enqueueUrlStats(UrlStatEntry stats, const Config& config) noexcept try {
        // Re-checked here even though span-side callers gate at SetUrlStat:
        // the config-loading overload above and any future callers still rely
        // on this as the authoritative drop point.
        if (!config.http.url_stat.enable) {
            return;
        }

        auto& shard = queueShard();
        int64_t prev_pending;
        {
            std::lock_guard<std::mutex> shard_lock(shard.mutex_);
            // The limit check uses its own relaxed load; concurrent enqueues
            // on other shards can overshoot it by at most kQueueShardCount
            // entries, which is harmless for a drop threshold. This bound is
            // url_stat.queue_size, not url_stat.limit: limit caps distinct
            // URL keys per snapshot, while queue_size caps the per-request
            // records buffered here awaiting aggregation.
            if (pending_.load(std::memory_order_relaxed) >= static_cast<int64_t>(config.http.url_stat.queue_size)) {
                LOG_DEBUG("drop url stats: overflow max queue size {}", config.http.url_stat.queue_size);
                return;
            }
            shard.queue_.push(std::move(stats));
            prev_pending = pending_.fetch_add(1, std::memory_order_relaxed);
        }

        // Wake the worker only on the empty→non-empty transition, decided by
        // the fetch_add return value: it is atomic with the increment, so the
        // transition cannot be missed. (A pending_ value loaded before the
        // push could go stale — the worker drains to zero and blocks between
        // the load and the push — and then no enqueue would ever notify
        // again, stranding the worker forever.) While entries are already
        // pending the worker is awake (or will re-check its predicate before
        // blocking), so the common case skips add_mutex_ entirely. Taking the
        // (empty) lock pairs the notify with the worker's predicate check, so
        // it cannot fire between the worker reading pending_ == 0 and
        // blocking — a lost wakeup that would strand the entry until the next
        // enqueue.
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

    void UrlStats::addUrlStatsWorker() {
        // Boot-time decision: CollectUrlStat is non-reloadable
        // (Config::retainNonReloadableFrom retains the whole http.url_stat
        // block on a reload), so a worker that returns here can never be
        // needed later — the enqueue path's authoritative drop point reads
        // the same never-changing flag, and no entry can strand in the
        // shards. If that flag is ever made reloadable, this gate (and
        // sendUrlStatsWorker's, and GrpcStats::sendStatsWorker's) must
        // become a per-cycle live check instead.
        const auto config = agent_->getConfig();
        if (!config->http.url_stat.enable) {
            return;
        }

        // Supervise the loop body so an unexpected exception (e.g. bad_alloc
        // while aggregating a snapshot) cannot kill URL-stat aggregation for
        // the process lifetime, mirroring the gRPC workers. Restarts are
        // paced by the send interval; only agent exit ends the worker.
        while (true) {
            try {
                runAddUrlStatsWorker(*config);
                break;
            } catch (const std::exception& e) {
                LOG_ERROR("add url stats worker exception = {}", e.what());
            } catch (...) {
                LOG_ERROR("add url stats worker unknown exception");
            }

            std::unique_lock<std::mutex> lock(add_mutex_);
            if (add_cond_var_.wait_for(lock, send_interval_, [this] { return agent_->isExiting(); })) {
                break;
            }
        }
        LOG_INFO("add url stats worker end");
    }

    void UrlStats::runAddUrlStatsWorker(const Config& config) {
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
            drainQueueShards(config);
            lock.lock();
        }
    }

    void UrlStats::stopAddUrlStatsWorker() {
        // Always notify, without consulting any config: re-deriving the
        // worker's boot decision from a config load here would couple
        // shutdown to the non-reloadability invariant documented in
        // addUrlStatsWorker — get it wrong and the only wakeup for a worker
        // parked in an untimed wait is skipped, hanging shutdown forever.
        // Taking the lock pairs the notify with the worker's predicate check
        // (isExiting), so it cannot fire in the window between the worker
        // reading the flag as false and blocking — a lost wakeup that would
        // also hang shutdown forever.
        std::lock_guard<std::mutex> lock(add_mutex_);
        add_cond_var_.notify_one();
    }

    void UrlStats::sendUrlStatsWorker() {
        // Boot-time decision; see addUrlStatsWorker for the
        // non-reloadability invariant this relies on.
        const auto config = agent_->getConfig();
        if (!config->http.url_stat.enable) {
            return;
        }

        // Supervised like addUrlStatsWorker: a transient exception must not
        // end periodic URL-stat sending for the process lifetime.
        while (true) {
            try {
                runSendUrlStatsWorker();
                break;
            } catch (const std::exception& e) {
                LOG_ERROR("send url stats worker exception = {}", e.what());
            } catch (...) {
                LOG_ERROR("send url stats worker unknown exception");
            }

            std::unique_lock<std::mutex> lock(send_mutex_);
            if (send_cond_var_.wait_for(lock, send_interval_, [this] { return agent_->isExiting(); })) {
                break;
            }
        }

        LOG_INFO("send url stats worker end");
    }

    void UrlStats::runSendUrlStatsWorker() {
        std::unique_lock<std::mutex> lock(send_mutex_);
        while (!agent_->isExiting()) {
            if (!send_cond_var_.wait_for(lock, send_interval_, [this]{ return agent_->isExiting(); })) {
                agent_->recordStats(URL_STATS);
            }
        }
    }

    void UrlStats::stopSendUrlStatsWorker() {
        // Same rationale as stopAddUrlStatsWorker: notify unconditionally and
        // under the lock. This worker's wait_for bounds a lost wakeup at the
        // send interval rather than forever, but shutdown should not stall at
        // all.
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_cond_var_.notify_one();
    }
}
