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

#include "utility.h"  // QueueDropReporter, behind LOG_WARN_THROTTLED

namespace pinpoint {

    // Log levels
    constexpr const char* LOG_LEVEL_DEBUG = "debug";
    constexpr const char* LOG_LEVEL_INFO = "info";
    constexpr const char* LOG_LEVEL_WARN = "warning";
    /// Second accepted spelling of LOG_LEVEL_WARN. Go/logrus parses both
    /// "warn" and "warning" and Java writes "WARN", so a level copied from
    /// either agent's configuration works here instead of being rejected as a
    /// typo and silently leaving the level alone. Input only: write() labels
    /// lines with LOG_LEVEL_WARN.
    constexpr const char* LOG_LEVEL_WARN_ALIAS = "warn";
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
         * @param max_backups Rotated files kept beside the live one
         *        (`<path>.1` .. `<path>.max_backups`, newest first). Values
         *        < 1 are treated as 1, the behaviour from before the setting
         *        existed.
         */
        void setFileLogger(const std::string& log_file_path, const int max_size,
                           const int max_backups = 1);
        /**
         * @brief Installs the host log sink, or clears it when @p sink is
         *        empty.
         *
         * A sink takes the line instead of the file/stdout sinks, so the host
         * sees no duplicates. See `pinpoint::LogSink` for the contract the
         * callback owes us; shutdown() clears the sink, because past that
         * point the host's logger may already be gone.
         */
        void setSink(LogSink sink);
        /// @brief Flushes pending log messages and releases file resources,
        /// and bans the std::cout fallback from there on (see `closed_`).
        /// Idempotent: the teardown paths call it more than once, and the
        /// last call is what closes the file a straggler reopened.
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
        bool openFileLocked();
        void rotateFileIfNeededLocked();

        std::string file_path_;
        std::uint64_t max_file_size_{0};
        int max_backups_{1};
        std::uint64_t current_file_size_{0};
        bool file_enabled_{false};
        // Set by shutdown(), cleared by setFileLogger(). It bans the
        // std::cout fallback rather than logging itself: the agent is
        // embedded in hosts (nginx) whose stdout must not be polluted by
        // stragglers that log after shutdown. A configured log file stays
        // available — write() reopens it in append mode, so the lines an
        // overrunning teardown emits on its way out are still recorded — and
        // a line is dropped only when there is no file to put it in, which is
        // the whole of the stdout-only configuration once the agent is gone.
        bool closed_{false};
        // Latched when the file sink fails in a way that must not degrade to
        // std::cout: a post-rotation reopen (the old file already renamed
        // away), or the post-shutdown reopen above. Unlike the open failure in
        // setFileLogger(), which is configuration time and keeps the fallback
        // so the host can see what went wrong, these strand a running agent
        // whose every later line would otherwise land on the host's stdout.
        // Cleared by setFileLogger().
        bool file_broken_{false};
        std::unique_ptr<std::ofstream> file_stream_;
        // Guarded by mutex_, like the file sink it replaces — and so called
        // with mutex_ held, which is what makes a sink that logs back into the
        // agent a self-deadlock (documented at pinpoint::LogSink). Checked
        // through sink_enabled_ so the no-sink path, which is every host that
        // does not set one, keeps building its line before taking the lock.
        LogSink sink_;
        std::atomic<bool> sink_enabled_{false};
        mutable std::mutex mutex_;
        std::atomic<int> current_level_{static_cast<int>(LogLevel::kInfo)};

        Logger() {}
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
    // static QueueDropReporter in its acquire() mode): at most one line per
    // QueueDropReporter::kDefaultReportInterval, with suppressed repeats
    // folded into the next granted line's count. Use for warnings that can
    // recur once per request on application threads — API misuse
    // (already-finished span touched again, depth/sequence overflow) and
    // malformed peer input (unparseable trace headers) — where an unthrottled
    // line per request would serialize the host's request threads on the
    // logger mutex and its per-line write+flush. Suppressed calls cost one
    // relaxed increment, one clock read and one relaxed load.
    #define LOG_WARN_THROTTLED(...) \
        do { \
            ::pinpoint::Logger& _pp_logger = ::pinpoint::Logger::getInstance(); \
            if (_pp_logger.warnEnabled()) { \
                static ::pinpoint::QueueDropReporter _pp_throttle; \
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
