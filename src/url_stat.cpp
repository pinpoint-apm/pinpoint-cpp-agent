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

#include <algorithm>
#include <mutex>
#include <thread>

#include "logging.h"
#include "url_stat.h"

namespace pinpoint {

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

    static std::string build_url_stat_key(const UrlStatEntry& us,
                                          const TrimmedUrlPath& trimmed,
                                          bool method_prefix_enabled) {
        // A prefix built from an empty method would leave a leading space
        // (" /api/users"), splitting the same URL into two server-side keys
        // depending on whether the method was known. Java's
        // UriMethodTransformer and Go's url_stat.go both skip the prefix in
        // that case; match them.
        const bool method_prefix = method_prefix_enabled && !us.method_.empty();
        const auto method_prefix_size = method_prefix ? us.method_.size() + 1 : 0;
        std::string url;
        url.reserve(method_prefix_size + trimmed.path.size() + (trimmed.wildcard ? 1 : 0));
        if (method_prefix) {
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
        if (us == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        addLocked(*us, config);
    }

    void UrlStats::addLocked(const UrlStatEntry& us, const Config& config) {
        // Tick boundary: the first entry of a newer tick closes the one in
        // progress. Entry arrival drives this rather than a timer thread —
        // entries carry an end time of about "now", so the cut lands on the
        // boundary anyway, and a tick with no traffic has nothing to cut.
        // Whatever is still in progress when a send comes is taken by
        // takeSnapshot(), so a trailing tick is never stranded here.
        //
        // Strictly-newer only: a straggler for an already-cut tick (drained
        // out of order across shards) must not cut again. It lands in the
        // current snapshot under its own tick key, which is what the server
        // aggregates by, and merge() folds it back together on send.
        const auto tick = tick_clock_.tick(us.end_time_);
        if (!snapshot_->empty() && tick > snapshot_->tick()) {
            if (completed_.size() >= kMaxCompletedSnapshots) {
                completed_.pop_front();
                if (const auto dropped = snapshot_drop_reporter_.record()) {
                    LOG_WARN("url stat snapshot queue overflow: {} tick(s) dropped in total "
                             "(max {} completed snapshots); the stats stream is not draining",
                             dropped, kMaxCompletedSnapshots);
                }
            }
            completed_.push_back(std::move(snapshot_));
            snapshot_ = std::make_unique<UrlStatSnapshot>();
        }

        if (!snapshot_->add(&us, config, tick_clock_)) {
            // Logged under snapshot_mutex_ on purpose: the reporter grants at
            // most one line per interval, so this cannot stall takeSnapshot
            // (the send path) more than momentarily once a minute, and the
            // alternative is plumbing a count out of a 64-entry batch loop.
            if (const auto dropped = limit_drop_reporter_.record()) {
                LOG_WARN("url stat limit reached: {} url(s) dropped in total "
                         "(max {} distinct urls per tick)",
                         dropped, config.http.url_stat.limit);
            }
        }
    }

    std::unique_ptr<UrlStatSnapshot> UrlStats::takeSnapshot() {
        // Allocate the replacement before touching snapshot_: if this throws
        // under memory pressure the member must stay intact — moving it out
        // first would leave it null after the caller's catch, and the next
        // addSnapshot()/drainQueueShards() would dereference a null pointer.
        auto fresh = std::make_unique<UrlStatSnapshot>();
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.swap(fresh);
        // Every retained tick goes out in one message: PAgentUriStat carries a
        // repeated eachUriStat and each entry stamps its own tick, so draining
        // one snapshot per send token would have taken 4 send intervals to
        // clear a backlog the stream is finally able to accept.
        while (!completed_.empty()) {
            fresh->merge(*completed_.front());
            completed_.pop_front();
        }
        return fresh;
    }

    int64_t TickClock::tick(const std::chrono::system_clock::time_point end_time) const {
        const auto end_millis = std::chrono::duration_cast<std::chrono::milliseconds>(end_time.time_since_epoch());
        const auto interval = std::chrono::milliseconds(interval_ * 1000);
        return (end_millis - end_millis % interval).count();
    }

    static int getBucket(int32_t elapsed) noexcept {
        // Histogram bucket upper bounds (in milliseconds); elapsed >= 8000ms
        // lands in the final bucket (index 7).
        static constexpr int32_t bounds[]{100, 300, 500, 1000, 3000, 5000, 8000};
        return static_cast<int>(
            std::upper_bound(std::begin(bounds), std::end(bounds), elapsed) - std::begin(bounds));
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

    void UrlStatHistogram::merge(const UrlStatHistogram& other) {
        total_ += other.total_;
        if (max_ < other.max_) {
            max_ = other.max_;
        }
        for (int i = 0; i < URL_STATS_BUCKET_SIZE; i++) {
            histogram_[i] += other.histogram_[i];
        }
    }

    bool UrlStatSnapshot::add(const UrlStatEntry* us, const Config& config, TickClock& tick_clock) {
        if (us == nullptr) {
            return true;
        }

        const auto tick = tick_clock.tick(us->end_time_);
        // An empty pattern would aggregate under an unreadable empty key on
        // the server; bucket it under the same stand-in Java/Go use.
        const std::string_view url = us->url_pattern_.empty()
            ? URL_STAT_UNKNOWN
            : std::string_view{us->url_pattern_};
        const auto trimmed = config.http.url_stat.enable_trim_path
            ? trim_url_path_view(url, config.http.url_stat.trim_path_depth)
            : TrimmedUrlPath{url, false};
        auto key = UrlKey{
            build_url_stat_key(*us, trimmed, config.http.url_stat.method_prefix),
            tick};
        LOG_DEBUG("url stats snapshot add : {}, {}", key.url_, tick);

        auto found = urlMap_.find(key);
        if (found == urlMap_.end()) {
            if (urlMap_.size() >= static_cast<size_t>(config.http.url_stat.limit)) {
                return false;
            }
            if (urlMap_.empty() && config.http.url_stat.limit > 0) {
                constexpr size_t kMaxInitialReserve = 4096;
                urlMap_.reserve(std::min(static_cast<size_t>(config.http.url_stat.limit), kMaxInitialReserve));
            }
            found = urlMap_.try_emplace(std::move(key)).first;
        }

        // The boundary UrlStats::addLocked cuts on. Only accepted entries
        // advance it, and by max rather than last, so an out-of-order
        // straggler cannot move it back and cut the same tick twice.
        if (tick > tick_) {
            tick_ = tick;
        }

        auto& stats = found->second;
        stats.total.add(us->elapsed_);
        if (us->failed_) {
            stats.fail.add(us->elapsed_);
        }
        return true;
    }

    void UrlStatSnapshot::merge(UrlStatSnapshot& other) {
        // Splices every node whose key is not already here — no rehashing of
        // the moved entries, no string copies.
        urlMap_.merge(other.urlMap_);
        // Only key collisions are left behind, i.e. the same url in the same
        // tick reached both snapshots (a straggler). Fold rather than drop.
        for (auto& [key, stat] : other.urlMap_) {
            auto& target = urlMap_[key];
            target.total.merge(stat.total);
            target.fail.merge(stat.fail);
        }
        other.urlMap_.clear();
        if (tick_ < other.tick_) {
            tick_ = other.tick_;
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
        // Shutdown gate; see the accepting_ declaration. Relaxed: a racing
        // enqueue that slips past the flip lands in a shard queue and is
        // simply never drained — bounded by queue_size, freed with this
        // object — so no ordering is needed.
        if (!accepting_.load(std::memory_order_relaxed)) {
            return;
        }

        auto& shard = queueShard();
        int64_t prev_pending = 0;
        bool dropped_now = false;
        {
            std::lock_guard<std::mutex> shard_lock(shard.mutex_);
            // The limit check uses its own relaxed load; concurrent enqueues
            // on other shards can overshoot it by at most kQueueShardCount
            // entries, which is harmless for a drop threshold. This bound is
            // url_stat.queue_size, not url_stat.limit: limit caps distinct
            // URL keys per snapshot, while queue_size caps the per-request
            // records buffered here awaiting aggregation.
            if (pending_.load(std::memory_order_relaxed) >= static_cast<int64_t>(config.http.url_stat.queue_size)) {
                dropped_now = true;
            } else {
                shard.queue_.push(std::move(stats));
                prev_pending = pending_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (dropped_now) {
            // Reported outside the shard lock — this is the request hot path
            // and the WARN below formats and writes. WARN so outage data loss
            // is visible at the default log level, rate-limited so a full
            // queue cannot log once per dropped request.
            if (const auto dropped = queue_drop_reporter_.record()) {
                LOG_WARN("url stat queue overflow: {} dropped in total (max queue size {})",
                         dropped, config.http.url_stat.queue_size);
            }
            return;
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

        // Hoisted out of the loop, and swapped only into shards that hold
        // something. The worker wakes on the empty→non-empty transition, so it
        // runs about once per enqueue under load; building a deque per shard
        // meant kQueueShardCount allocations per call — nearly all of them for
        // shards that were empty — to move a handful of entries. Reuse is safe
        // because the drain loop below always leaves batch empty, and keeping
        // its buffer is what removes the allocation. A non-empty shard gets
        // that buffer in exchange, which its next enqueue reuses.
        std::queue<UrlStatEntry> batch;

        for (auto& shard : queue_shards_) {
            {
                std::lock_guard<std::mutex> shard_lock(shard.mutex_);
                if (shard.queue_.empty()) {
                    continue;
                }
                batch.swap(shard.queue_);
                pending_.fetch_sub(static_cast<int64_t>(batch.size()), std::memory_order_relaxed);
            }

            while (!batch.empty()) {
                std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
                for (size_t i = 0; i < kEntriesPerSnapshotLock && !batch.empty(); i++) {
                    auto us = std::move(batch.front());
                    batch.pop();
                    addLocked(us, config);
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

        // Supervised (see superviseWorker), mirroring the gRPC workers.
        // Restarts are paced by the send interval; only agent exit ends it.
        superviseWorker("add url stats worker", send_interval_, add_mutex_, add_cond_var_,
                        [this] { return agent_->isExiting(); },
                        [&] { runAddUrlStatsWorker(*config); return true; });
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
        // Refuse new entries from here on (see accepting_): shutdown has
        // begun, so anything enqueued now would never be drained.
        accepting_.store(false, std::memory_order_relaxed);
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
        superviseWorker("send url stats worker", send_interval_, send_mutex_, send_cond_var_,
                        [this] { return agent_->isExiting(); },
                        [this] { runSendUrlStatsWorker(); return true; });
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
