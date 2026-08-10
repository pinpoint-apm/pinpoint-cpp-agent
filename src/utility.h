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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <optional>
#include <thread>
#include <vector>
#include <string_view>

#include "pinpoint/tracer.h"  // pinpoint::SqlUid

namespace pinpoint {

    /**
     * @brief Generates a pseudo-random span identifier.
     *
     * @return 64-bit span ID. As with any finite random identifier, collisions
     *         are possible.
     */
    int64_t generate_span_id();
    /**
     * @brief Converts a system clock time point to epoch milliseconds.
     *
     * @param tm Time point to convert.
     * @return Milliseconds since epoch.
     */
    int64_t to_milli_seconds(const std::chrono::system_clock::time_point& tm);

    /**
     * @brief Produces a deterministic UID for a normalized SQL string.
     *
     * @param sql Normalized SQL string.
     * @return 16-byte MurmurHash3 fingerprint. Different SQL strings can
     *         theoretically collide.
     */
    SqlUid generate_sql_uid(std::string_view sql);

    /// @brief Returns the host name of the running process.
    std::string get_host_name();
    /// @brief Returns the host IP address of the running process.
    std::string get_host_ip_addr();

    /// @brief Safe string-to-int conversion returning `std::nullopt` on error.
    std::optional<int> stoi_(std::string_view str);
    /// @brief Safe string-to-int64 conversion returning `std::nullopt` on error.
    std::optional<int64_t> stoll_(std::string_view str);
    /// @brief Safe string-to-double conversion returning `std::nullopt` on error.
    std::optional<double> stod_(std::string_view str);
    /// @brief Safe string-to-bool conversion returning `std::nullopt` on error.
    std::optional<bool> stob_(std::string_view str);

    /**
     * @brief Abandons a thread handle without joining or detaching it,
     *        tolerating handles inherited across fork().
     *
     * The joinable object's storage is reused for a default-constructed empty
     * std::thread without invoking the old object's destructor. This leaves
     * @p t non-joinable without allocating memory or making a pthread call.
     * detach() must never be used here: for a handle inherited across fork()
     * the thread does not exist in the child, and while macOS pthread_detach
     * fails cleanly with ESRCH (making std::thread::detach() throw catchably),
     * glibc unconditionally dereferences the thread descriptor — memory the
     * child's fork() has already reclaimed — and crashes. Cost of the leak: a
     * LIVE abandoned thread keeps its native resources until process exit; a
     * fork-dead handle leaks only the stale handle state.
     */
    void abandon_thread(std::thread& t) noexcept;

    /**
     * @brief Returns the largest length <= max_len that does not split a
     * multibyte UTF-8 sequence.
     *
     * A byte-count cut landing mid-character would produce invalid UTF-8,
     * which flows into protobuf string fields (SQL metadata, callstack
     * frames) that require valid UTF-8 — the collector may reject or mangle
     * such data. Walks back over at most 3 continuation bytes; if the lead
     * byte's sequence runs past the cut, the whole partial character is
     * dropped. Malformed input (stray continuation bytes) is trimmed
     * conservatively, never extended.
     */
    size_t utf8SafeCutLength(std::string_view s, size_t max_len);

    /**
     * @brief Rate-limited reporter for queue-overflow drops.
     *
     * Producers call record() on every dropped item, from any thread; at most
     * one report per interval is granted, carrying the cumulative drop count.
     * This mirrors the span queue's once-per-interval drop report
     * (GrpcSpan::maybe_log_span_queue_drops) for the metadata, stats and
     * url-stat queues: during a collector outage an unlimited WARN per
     * dropped item would flood the log from request threads, while the
     * previous per-drop DEBUG line was invisible at the default level —
     * outage data loss went unreported either way.
     */
    class QueueDropReporter {
    public:
        static constexpr std::chrono::seconds kDefaultReportInterval{60};

        explicit QueueDropReporter(
            std::chrono::steady_clock::duration interval = kDefaultReportInterval)
            : interval_(interval) {}

        /**
         * @brief Counts one dropped item.
         *
         * @return The cumulative drop count when this caller won the current
         *         report window and should log it now; 0 while rate-limited
         *         (the drop is still counted and folded into the next report).
         *         The very first drop always reports.
         */
        uint64_t record() noexcept {
            const auto dropped = dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            auto next = next_report_at_.load(std::memory_order_relaxed);
            if (now < next) {
                return 0;
            }
            // One concurrent producer wins the window via the CAS; the losers
            // stay silent and their drops surface in the winner's next count.
            if (next_report_at_.compare_exchange_strong(
                    next, now + interval_.count(), std::memory_order_relaxed)) {
                return dropped;
            }
            return 0;
        }

        /**
         * @brief Pull-side variant for queues that count their drops
         *        internally (the span queue: ShardedBoundedQueue counts
         *        overwritten-oldest drops under its shard locks, keeping the
         *        enqueue hot path free of clock reads and logging).
         *
         * The consumer polls with the queue's cumulative drop count and gets
         * it back when a report is due — the count grew since the last report
         * and the interval has passed — or 0 to stay silent. An instance is
         * either producer-counted (record()) or queue-counted (this); do not
         * mix the two on one instance, their counters are unrelated.
         */
        uint64_t report_if_due(uint64_t cumulative_dropped) noexcept {
            if (cumulative_dropped <= last_reported_.load(std::memory_order_relaxed)) {
                return 0;
            }
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            auto next = next_report_at_.load(std::memory_order_relaxed);
            if (now < next) {
                return 0;
            }
            if (next_report_at_.compare_exchange_strong(
                    next, now + interval_.count(), std::memory_order_relaxed)) {
                last_reported_.store(cumulative_dropped, std::memory_order_relaxed);
                return cumulative_dropped;
            }
            return 0;
        }

    private:
        const std::chrono::steady_clock::duration interval_;
        std::atomic<uint64_t> dropped_{0};
        // Cumulative count at the last granted report_if_due() report.
        std::atomic<uint64_t> last_reported_{0};
        std::atomic<std::chrono::steady_clock::rep> next_report_at_{0};
    };

    /**
     * @brief Supervises a background worker: runs `body`, and if it escapes
     * with an exception, logs it and restarts the body after `interval` (or
     * immediately ends on exit). A normal return ends the worker. Logs
     * "<name> end" when the worker finishes, so an unexpected exception
     * (e.g. bad_alloc while aggregating) cannot kill a periodic worker for
     * the process lifetime.
     *
     * @param name Worker name used in the log messages.
     * @param interval Restart pacing between supervised runs.
     * @param mutex / cond_var The worker's own synchronization pair, so the
     *        restart wait wakes on the worker's stop notification.
     * @param is_exiting Predicate consulted by the restart wait.
     * @param body One supervised run of the worker loop.
     *
     * Defined in utility.cpp (std::function is fine on this cold path) so
     * this header does not drag logging.h/fmt into every includer.
     */
    void superviseWorker(std::string_view name, std::chrono::milliseconds interval,
                         std::mutex& mutex, std::condition_variable& cond_var,
                         const std::function<bool()>& is_exiting,
                         const std::function<void()>& body);

    /**
     * @brief Lazily heap-creates one instance per thread (and per call site)
     * via `make` (returning a `new`ed T*) and caches it in a
     * trivially-destructible TLS slot, with a separate guard reclaiming it at
     * thread exit.
     *
     * Split this way so calls during thread teardown stay defined behavior: a
     * block-scope thread_local with a destructor must not be passed through
     * again once destroyed ([basic.start.term]) — yet a host thread_local
     * destructor that records a final span during thread exit re-enters these
     * paths exactly then (TLS destruction runs in reverse construction order,
     * so a host object constructed before this slot's first use is destroyed
     * after it). The slot has no destructor, so it is never "destroyed" and
     * stays valid for the whole thread lifetime; only the object it points at
     * is reclaimed, by the guard. Once the guard has run, a re-entering call
     * gets a replacement that is deliberately leaked — bounded at one per
     * thread that re-enters during its own exit. An exception from `make`
     * propagates and nothing is cached.
     */
    template <typename T, typename Make>
    T& thread_local_lazy(Make&& make) {
        struct Slot {
            T* value = nullptr;
            bool reclaimed = false;  // set once the reclaim guard has run
        };
        static thread_local Slot slot;
        if (slot.value == nullptr) {
            slot.value = make();
            if (!slot.reclaimed) {
                // Normal first use on this thread: register the guard that
                // reclaims the object at thread exit. (Unreachable once TLS
                // destructors have started — hence the reclaimed leak path.)
                struct Reclaim {
                    ~Reclaim() {
                        delete slot.value;
                        slot.value = nullptr;
                        slot.reclaimed = true;
                    }
                };
                static thread_local Reclaim reclaim;
                (void)reclaim;
            }
        }
        return *slot.value;
    }

}
