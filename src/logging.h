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
#include <cstdint>
#include <fstream>
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
         * @param log_file_path Path to the log file.
         * @param max_size Maximum file size (bytes) before rotation.
         */
        void setFileLogger(const std::string& log_file_path, const int max_size);
        /**
         * @brief Flushes pending log messages and releases file resources.
         */
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

        template <typename... Args>
        void log(LogLevel level, std::string_view file, int line, fmt::string_view format, Args&&... args) {
            if (!shouldLog(level)) {
                return;
            }
            std::string message;
            try {
                message = fmt::vformat(format, fmt::make_format_args(args...));
            } catch (const std::exception& e) {
                message = fmt::format("log format error: {}", e.what());
            }
            write(level, file, line, message);
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
     * @brief Flushes pending log messages and releases logger resources.
     */
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
    /// @brief Writes a debug-level log entry using the global logger.
    #define LOG_DEBUG(...) \
        do { \
            ::pinpoint::Logger& _pp_logger = ::pinpoint::Logger::getInstance(); \
            if (_pp_logger.debugEnabled()) { \
                _pp_logger.logDebug(::pinpoint::kFileName(__FILE__), __LINE__, __VA_ARGS__); \
            } \
        } while (0)
    /// @brief Writes an info-level log entry using the global logger.
    #define LOG_INFO(...) \
        do { \
            ::pinpoint::Logger& _pp_logger = ::pinpoint::Logger::getInstance(); \
            if (_pp_logger.infoEnabled()) { \
                _pp_logger.logInfo(::pinpoint::kFileName(__FILE__), __LINE__, __VA_ARGS__); \
            } \
        } while (0)
    /// @brief Writes a warning-level log entry using the global logger.
    #define LOG_WARN(...) \
        do { \
            ::pinpoint::Logger& _pp_logger = ::pinpoint::Logger::getInstance(); \
            if (_pp_logger.warnEnabled()) { \
                _pp_logger.logWarn(::pinpoint::kFileName(__FILE__), __LINE__, __VA_ARGS__); \
            } \
        } while (0)
    /// @brief Writes an error-level log entry using the global logger.
    #define LOG_ERROR(...) \
        do { \
            ::pinpoint::Logger& _pp_logger = ::pinpoint::Logger::getInstance(); \
            if (_pp_logger.errorEnabled()) { \
                _pp_logger.logError(::pinpoint::kFileName(__FILE__), __LINE__, __VA_ARGS__); \
            } \
        } while (0)
}
