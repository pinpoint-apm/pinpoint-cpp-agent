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
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <mutex>
#include <utility>
#include <fmt/format.h>

namespace pinpoint {

    // Log levels
    constexpr const char* LOG_LEVEL_DEBUG = "debug";
    constexpr const char* LOG_LEVEL_INFO = "info";
    constexpr const char* LOG_LEVEL_WARN = "warning";
    constexpr const char* LOG_LEVEL_ERROR = "error";

    /**
     * @brief Thread-safe singleton wrapper around internal logging.
     *
     * `Logger` centralizes log configuration so the rest of the agent can write messages
     * without having to manage logger instances or sinks.
     */
    class Logger {
    public:
        /// Deleted copy-constructor to enforce the singleton contract.
        Logger(const Logger &) = delete;
        /// Deleted assignment operator to enforce the singleton contract.
        Logger &operator=(const Logger &) = delete;

        /**
         * @brief Returns the lazily-constructed global logger instance.
         *
         * The first call creates the underlying logger and subsequent calls reuse it.
         *
         * @return Reference to the global `Logger`.
         */
        static Logger& getInstance() {
            // Intentionally heap-allocated and never destroyed. Background
            // threads (gRPC workers, config watcher, async RPC callbacks) may
            // log during process exit; a function-local static Logger would be
            // destroyed by then, turning those calls into use-after-destruction
            // of the mutex and file stream. Graceful flush still happens via
            // shutdown_logger() on the explicit Shutdown() path.
            static auto* instance = new Logger();
            return *instance;
        }

        /**
         * @brief Adjusts the log level for runtime diagnostics.
         *
         * @param log_level One of the level strings accepted by the agent.
         */
        void setLogLevel(const std::string& log_level);
        /**
         * @brief Switches the logger to file output with basic log rotation support.
         *
         * @param log_file_path Path to the log file. An empty path disables
         *        file output and switches back to stdout.
         * @param max_size Maximum file size (MB) before rotation; <= 0
         *        disables rotation.
         */
        void setFileLogger(const std::string& log_file_path, const int max_size);
        /// @brief Flushes pending log messages and releases file resources.
        void shutdown();

        template <typename... Args>
        void logDebug(std::string_view file, int line, fmt::string_view format, Args&&... args) {
            log(LogLevel::kDebug, file, line, format, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void logInfo(std::string_view file, int line, fmt::string_view format, Args&&... args) {
            log(LogLevel::kInfo, file, line, format, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void logWarn(std::string_view file, int line, fmt::string_view format, Args&&... args) {
            log(LogLevel::kWarn, file, line, format, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void logError(std::string_view file, int line, fmt::string_view format, Args&&... args) {
            log(LogLevel::kError, file, line, format, std::forward<Args>(args)...);
        }

        /// @brief Variant behind the LOG_WARN_THROTTLED macro: identical to
        /// logWarn, except that when `occurrences` > 1 the line notes how
        /// many duplicates the call site's throttle folded into it.
        template <typename... Args>
        void logWarnThrottled(uint64_t occurrences, std::string_view file, int line,
                              fmt::string_view format, Args&&... args) {
            log(LogLevel::kWarn, occurrences, file, line, format, std::forward<Args>(args)...);
        }

        /// @brief Level predicates used by the LOG_* macros to skip argument
        /// evaluation entirely when the level is disabled.
        bool debugEnabled() const { return shouldLog(LogLevel::kDebug); }
        bool infoEnabled() const { return shouldLog(LogLevel::kInfo); }
        bool warnEnabled() const { return shouldLog(LogLevel::kWarn); }
        bool errorEnabled() const { return shouldLog(LogLevel::kError); }

    private:
        enum class LogLevel : int {
            kDebug = 0,
            kInfo = 1,
            kWarn = 2,
            kError = 3,
        };

        // Must never throw: LOG_* is called from the catch handlers of the
        // worker threads, where an escaping exception (e.g. bad_alloc while
        // formatting or writing the message) would leave the thread function
        // through a handler and std::terminate() the host process. The outer
        // catch-all covers write() and the fallback fmt::format itself, both
        // of which allocate.
        template <typename... Args>
        void log(LogLevel level, std::string_view file, int line, fmt::string_view format, Args&&... args) noexcept {
            log(level, 1, file, line, format, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void log(LogLevel level, uint64_t occurrences, std::string_view file, int line,
                 fmt::string_view format, Args&&... args) noexcept {
            if (!shouldLog(level)) {
                return;
            }
            try {
                std::string message;
                try {
                    message = fmt::vformat(format, fmt::make_format_args(args...));
                } catch (const std::exception& e) {
                    message = fmt::format("log format error: {}", e.what());
                }
                if (occurrences > 1) {
                    // Duplicates a LOG_*_THROTTLED site suppressed since its
                    // previous report, folded into this line.
                    fmt::format_to(std::back_inserter(message),
                                   " [{} occurrences since last report]", occurrences);
                }
                write(level, file, line, message);
            } catch (...) {
                // Drop the message: logging failure must stay invisible to
                // the host application.
            }
        }

        bool shouldLog(LogLevel level) const {
            return static_cast<int>(level) >= current_level_.load(std::memory_order_relaxed);
        }

        void write(LogLevel level, std::string_view file, int line, const std::string& message);
        void rotateFileIfNeededLocked();

        std::string file_path_;
        std::uint64_t max_file_size_{0};
        std::uint64_t current_file_size_{0};
        bool file_enabled_{false};
        std::unique_ptr<std::ofstream> file_stream_;
        mutable std::mutex mutex_;
        std::atomic<int> current_level_{static_cast<int>(LogLevel::kInfo)};

        Logger() {}
    };

    /**
     * @brief Per-call-site rate limiter behind the LOG_*_THROTTLED macros.
     *
     * Some misuse/malformed-input warnings can fire once per request on
     * application threads (a finished span touched again, a call tree over the
     * depth limit, a malformed trace header). Every emitted line takes the
     * process-global logger mutex and does a synchronous write+flush, so an
     * application that keeps hitting one serializes its request threads on the
     * logger. Mirrors QueueDropReporter (utility.h): at most one line per
     * interval, with suppressed repeats folded into the next granted line.
     */
    class LogSiteThrottle {
    public:
        static constexpr std::chrono::seconds kDefaultInterval{60};

        constexpr explicit LogSiteThrottle(
            std::chrono::steady_clock::duration interval = kDefaultInterval)
            : interval_(interval.count()) {}

        /**
         * @brief Counts one occurrence at the call site.
         *
         * @return The number of occurrences this report covers (this one
         *         plus everything suppressed since the previous report)
         *         when the caller won the current interval and should log
         *         now; 0 while rate-limited. The very first occurrence
         *         always reports.
         */
        uint64_t acquire() noexcept {
            occurrences_.fetch_add(1, std::memory_order_relaxed);
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            auto next = next_report_at_.load(std::memory_order_relaxed);
            if (now < next) {
                return 0;
            }
            // One concurrent caller wins the interval via the CAS; the
            // losers stay silent and their occurrences surface in the
            // winner's count.
            if (next_report_at_.compare_exchange_strong(next, now + interval_,
                                                        std::memory_order_relaxed)) {
                return occurrences_.exchange(0, std::memory_order_relaxed);
            }
            return 0;
        }

    private:
        std::chrono::steady_clock::rep interval_;
        std::atomic<uint64_t> occurrences_{0};
        std::atomic<std::chrono::steady_clock::rep> next_report_at_{0};
    };

    /// @brief Flushes pending log messages and releases logger resources.
    void shutdown_logger();

    constexpr static std::string_view kFileName(std::string_view path) {
        size_t last_slash = path.find_last_of("\\/");
        return (last_slash == std::string_view::npos) ? path : path.substr(last_slash + 1);
    }

    // The level is checked before the variadic arguments are evaluated, so
    // formatting work (and the cost of the argument expressions themselves) is
    // skipped entirely for disabled levels — notably LOG_DEBUG, which is off
    // by default yet appears on hot paths. Arguments must therefore be free of
    // side effects, which holds across the codebase.
    /// @brief Common body of the leveled LOG_* macros: check the level
    /// predicate, then forward to the matching Logger method.
    #define PP_LOG(predicate, method, ...) \
        do { \
            ::pinpoint::Logger& _pp_logger = ::pinpoint::Logger::getInstance(); \
            if (_pp_logger.predicate()) { \
                _pp_logger.method(::pinpoint::kFileName(__FILE__), __LINE__, __VA_ARGS__); \
            } \
        } while (0)
    /// @brief Writes a debug-level log entry using the global logger.
    #define LOG_DEBUG(...) PP_LOG(debugEnabled, logDebug, __VA_ARGS__)
    /// @brief Writes an info-level log entry using the global logger.
    #define LOG_INFO(...) PP_LOG(infoEnabled, logInfo, __VA_ARGS__)
    /// @brief Writes a warning-level log entry using the global logger.
    #define LOG_WARN(...) PP_LOG(warnEnabled, logWarn, __VA_ARGS__)
    /// @brief Writes an error-level log entry using the global logger.
    #define LOG_ERROR(...) PP_LOG(errorEnabled, logError, __VA_ARGS__)

    // LOG_WARN variant rate-limited per call site (each expansion owns a
    // static LogSiteThrottle): at most one line per
    // LogSiteThrottle::kDefaultInterval, with suppressed repeats folded into
    // the next granted line's count. Use for warnings that can recur once per
    // request on application threads — API misuse (already-finished span
    // touched again, depth/sequence overflow) and malformed peer input
    // (unparseable trace headers) — where an unthrottled line per request
    // would serialize the host's request threads on the logger mutex and its
    // per-line write+flush. Suppressed calls cost one relaxed increment, one
    // clock read and one relaxed load.
    #define LOG_WARN_THROTTLED(...) \
        do { \
            ::pinpoint::Logger& _pp_logger = ::pinpoint::Logger::getInstance(); \
            if (_pp_logger.warnEnabled()) { \
                static ::pinpoint::LogSiteThrottle _pp_throttle; \
                const auto _pp_occurrences = _pp_throttle.acquire(); \
                if (_pp_occurrences > 0) { \
                    _pp_logger.logWarnThrottled(_pp_occurrences, ::pinpoint::kFileName(__FILE__), __LINE__, __VA_ARGS__); \
                } \
            } \
        } while (0)

    // Exception boundary for host-facing entry points (the Span/SpanEvent
    // recording surface): expands to the catch clauses of a function-try-block
    // and degrades the call to a logged no-op. These functions are called
    // straight from host request handlers and destructors, where an escaping
    // exception (realistically bad_alloc from a string/annotation allocation)
    // would unwind host frames or std::terminate the process — nothing may
    // escape, matching the pt_api_call firewall on the C API. `op_desc` must
    // be a string literal (it must not be named `what`, or it would be
    // substituted into the `_pp_e.what()` call below).
    #define CATCH_AND_LOG(op_desc) \
        catch (const std::exception& _pp_e) { \
            LOG_ERROR(op_desc " exception = {}", _pp_e.what()); \
        } catch (...) { \
            LOG_ERROR(op_desc " unknown exception"); \
        }
    /// @brief CATCH_AND_LOG for value-returning entry points.
    #define CATCH_AND_LOG_RETURN(op_desc, retval) \
        catch (const std::exception& _pp_e) { \
            LOG_ERROR(op_desc " exception = {}", _pp_e.what()); \
            return (retval); \
        } catch (...) { \
            LOG_ERROR(op_desc " unknown exception"); \
            return (retval); \
        }
}
