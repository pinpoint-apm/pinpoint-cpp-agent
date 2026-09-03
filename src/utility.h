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
#include <sys/types.h>
#include <thread>
#include <string_view>

#if defined(__SANITIZE_ADDRESS__)
#define PINPOINT_HAS_LSAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PINPOINT_HAS_LSAN 1
#endif
#endif

#if defined(PINPOINT_HAS_LSAN)
#include <sanitizer/lsan_interface.h>
#endif

#include "pinpoint/tracer.h"  // pinpoint::SqlUid

namespace pinpoint {

    /// @brief Pseudo-random 64-bit span id; collisions are possible.
    int64_t generate_span_id();
    /// @brief Converts a system clock time point to epoch milliseconds.
    int64_t to_milli_seconds(const std::chrono::system_clock::time_point& tm);

    /// @brief Deterministic 16-byte MurmurHash3 UID for a normalized SQL
    ///        string; distinct strings can theoretically collide.
    SqlUid generate_sql_uid(std::string_view sql);

    std::string get_host_name();
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
     * @brief Returns the calling process's id, served from a cache that a
     *        fork handler keeps current.
     *
     * The fork-inheritance guards compare a stored owner pid against the
     * running process on paths as hot as span admission (once per NewSpan), and
     * getpid() is not uniformly cheap: glibc dropped its pid cache in 2.25 and
     * the call is not in the vDSO, so on Linux it is a real syscall (~139 ns
     * against ~1 ns for the relaxed atomic load here; Darwin still caches it at
     * ~2 ns). Reading it from here costs the same everywhere.
     *
     * Correctness across fork() comes from a pthread_atfork child handler,
     * which runs before fork() returns in the child, so the child never
     * observes the parent's pid. If that handler cannot be registered the cache
     * is bypassed and every call goes to getpid(): slower, never stale.
     */
    pid_t current_pid() noexcept;

    /**
     * @brief Abandons a thread handle without joining or detaching it,
     *        tolerating handles inherited across fork().
     *
     * The joinable object's storage is reused for a default-constructed empty
     * std::thread without running the old object's destructor, leaving @p t
     * non-joinable with no allocation and no pthread call. detach() must never
     * be used here: for a handle inherited across fork() the thread does not
     * exist in the child, and while macOS pthread_detach fails cleanly with
     * ESRCH, glibc unconditionally dereferences the thread descriptor — memory
     * the child's fork() already reclaimed — and crashes. Cost of the leak: a
     * LIVE abandoned thread keeps its native resources until process exit; a
     * fork-dead handle leaks only stale handle state.
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

    /// @brief Cap on the error string carried by a span or span event,
    ///        matching the 256 Java passes to StringUtils.abbreviate().
    inline constexpr size_t kMaxErrorStringLength = 256;

    /**
     * @brief Abbreviates an error message the way Java's
     * StringUtils.abbreviate(msg, 256) does.
     *
     * Java's AbstractRecorder.recordException stores
     * StringUtils.abbreviate(throwable.getMessage(), 256): a message within
     * the cap is kept verbatim, a longer one keeps its first 256 and gains a
     * "...(<original length>)" suffix naming the length that was dropped.
     * Uncapped, a single driver error carrying a whole SQL statement inflates
     * every span that records it.
     *
     * The cut is by byte, not by Java char — that is the unit the payload is
     * billed in, the unit the agent's other truncations (SQL, callstack) use,
     * and identical to Java for ASCII messages. utf8SafeCutLength keeps the
     * cut off a multibyte boundary so the result stays valid UTF-8 for
     * protobuf.
     */
    std::string abbreviateErrorString(std::string_view msg);

    /**
     * @brief Rate-limited event reporter: at most one report per interval, no
     *        matter how many threads call in.
     *
     * Without it an unlimited WARN per event would flood the log from request
     * threads (and serialize them on the logger's mutex and per-line flush),
     * while a DEBUG line would be invisible at the default level — either way
     * the event goes unreported. One caller wins each interval via a CAS; the
     * losers stay silent and their events surface in the winner's count.
     *
     * Pick ONE of the three counting modes per instance and use only that
     * one — they keep unrelated counters:
     *   - record()          producers count each event; reports the running total
     *   - report_if_due(n)  the producer counts internally; poll with its total
     *   - acquire()         reports how many events this line covers, then resets
     *
     * The constructor is constexpr so a function-local static instance (see
     * LOG_WARN_THROTTLED) is constant-initialized, with no thread-safe-static
     * guard on the suppressed path.
     */
    class QueueDropReporter {
    public:
        static constexpr std::chrono::seconds kDefaultReportInterval{60};

        constexpr explicit QueueDropReporter(
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
            return win_window() ? dropped : 0;
        }

        /**
         * @brief Counts one occurrence at a throttled log call site.
         *
         * @return How many occurrences this report covers (this one plus
         *         everything suppressed since the previous report) when the
         *         caller won the current interval and should log now; 0 while
         *         rate-limited. The very first occurrence always reports.
         *
         * Unlike record(), the counter resets per report, so the granted line
         * describes the interval it closes rather than all time.
         */
        uint64_t acquire() noexcept {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return win_window() ? dropped_.exchange(0, std::memory_order_relaxed) : 0;
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
            if (!win_window()) {
                return 0;
            }
            last_reported_.store(cumulative_dropped, std::memory_order_relaxed);
            return cumulative_dropped;
        }

    private:
        /// @brief True for the single caller that claims the current interval.
        bool win_window() noexcept {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            auto next = next_report_at_.load(std::memory_order_relaxed);
            if (now < next) {
                return false;
            }
            return next_report_at_.compare_exchange_strong(
                next, now + interval_.count(), std::memory_order_relaxed);
        }

        const std::chrono::steady_clock::duration interval_;
        std::atomic<uint64_t> dropped_{0};
        // Cumulative count at the last granted report_if_due() report.
        std::atomic<uint64_t> last_reported_{0};
        std::atomic<std::chrono::steady_clock::rep> next_report_at_{0};
    };

    /**
     * @brief Supervises a background worker: runs `body`, and if it escapes
     * with an exception, logs it and restarts the body after `interval` (or
     * immediately ends on exit). Logs "<name> end" when the worker finishes,
     * so an unexpected exception (e.g. bad_alloc while aggregating) cannot
     * kill a periodic worker for the process lifetime.
     *
     * @param name Worker name used in the log messages.
     * @param interval Restart pacing between supervised runs.
     * @param mutex / cond_var The worker's own synchronization pair, so the
     *        restart wait wakes on the worker's stop notification.
     * @param is_exiting Predicate consulted by the restart wait.
     * @param body One supervised run of the worker loop. Returns true when
     *        the worker is done, or false to be restarted after `interval`
     *        exactly like the exception paths — the stream workers use that
     *        for a stream that failed to start, which is expected rather
     *        than exceptional.
     * @param on_error Optional cleanup run inside the exception handlers,
     *        before the restart wait (the stream workers drain their broken
     *        stream here).
     *
     * Defined in utility.cpp (std::function is fine on this cold path) so
     * this header does not drag logging.h/fmt into every includer.
     */
    void superviseWorker(std::string_view name, std::chrono::milliseconds interval,
                         std::mutex& mutex, std::condition_variable& cond_var,
                         const std::function<bool()>& is_exiting,
                         const std::function<bool()>& body,
                         const std::function<void()>& on_error = {});

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
#if defined(PINPOINT_HAS_LSAN)
            } else {
                // Re-entry after the TLS reclaim guard has run intentionally
                // keeps this replacement alive until the OS releases the
                // thread. Tell LeakSanitizer about that documented lifetime so
                // it still reports every other unreachable allocation.
                __lsan_ignore_object(slot.value);
#endif
            }
        }
        return *slot.value;
    }

}

#if defined(PINPOINT_HAS_LSAN)
#undef PINPOINT_HAS_LSAN
#endif
