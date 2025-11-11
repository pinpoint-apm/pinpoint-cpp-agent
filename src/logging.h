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

#include <memory>
#include <string>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace pinpoint {

    /**
     * @brief Thread-safe singleton wrapper around the internal `spdlog` logger.
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
            // Singleton
            std::call_once(init_flag_, []() {
                instance_.reset(new Logger);
            });
            return *(instance_);
        }

        /**
         * @brief Returns the underlying `spdlog` logger instance.
         *
         * @return Shared pointer to the configured `spdlog::logger`.
         */
        std::shared_ptr<spdlog::logger>& getLogger() { return logger_; }
        /**
         * @brief Adjusts the log level for runtime diagnostics.
         *
         * @param log_level One of the level strings accepted by `spdlog`.
         */
        void setLogLevel(const std::string& log_level) const;
        /**
         * @brief Switches the logger to file output with log rotation support.
         *
         * @param log_file_path Path to the log file.
         * @param max_size Maximum file size (bytes) before rotation.
         */
        void setFileLogger(const std::string& log_file_path, const int max_size);

    private:
        static std::unique_ptr<Logger> instance_;
        static std::once_flag init_flag_;
        std::shared_ptr<spdlog::logger>	logger_;

        Logger() : logger_(spdlog::stdout_color_mt("pinpoint")) {}
    };

    /**
     * @brief Initializes the global logging sink with default console output.
     */
    void init_logger();
    /**
     * @brief Flushes pending log messages and releases logger resources.
     */
    void shutdown_logger();

    /// @brief Writes a debug-level log entry using the global logger.
    #define LOG_DEBUG(...) (Logger::getInstance().getLogger()->debug(__VA_ARGS__))
    /// @brief Writes an info-level log entry using the global logger.
    #define LOG_INFO(...) (Logger::getInstance().getLogger()->info(__VA_ARGS__))
    /// @brief Writes a warning-level log entry using the global logger.
    #define LOG_WARN(...) (Logger::getInstance().getLogger()->warn(__VA_ARGS__))
    /// @brief Writes an error-level log entry using the global logger.
    #define LOG_ERROR(...) (Logger::getInstance().getLogger()->error(__VA_ARGS__))
}
