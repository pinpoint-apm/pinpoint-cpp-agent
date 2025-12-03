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

#include "spdlog/sinks/rotating_file_sink.h"
#include "logging.h"
#include <vector>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

namespace pinpoint {

    Logger::Logger() {
        // Default to stdout sink initially
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        logger_ = std::make_shared<spdlog::logger>("pinpoint", console_sink);
        // Default level
        logger_->set_level(spdlog::level::info);
        // Register to spdlog global registry for potential access
        spdlog::register_logger(logger_);
    }

    void init_logger() {
        Logger::getInstance().setLogLevel(LOG_LEVEL_INFO);
    }

    void shutdown_logger() {
        spdlog::shutdown();
    }

    void Logger::setLogLevel(const std::string& log_level) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto level = log_level.c_str();
        if (!strcasecmp(level, LOG_LEVEL_DEBUG)) {
            logger_->set_level(spdlog::level::debug);
        } else if (!strcasecmp(level, LOG_LEVEL_INFO)) {
            logger_->set_level(spdlog::level::info);
        } else if (!strcasecmp(level, LOG_LEVEL_WARN)) {
            logger_->set_level(spdlog::level::warn);
        } else if (!strcasecmp(level, LOG_LEVEL_ERROR)) {
            logger_->set_level(spdlog::level::err);
        }
    }

    void Logger::setFileLogger(const std::string& log_file_path, const int max_size) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Prepare new sink
        const auto size = max_size * 1024 * 1024;
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_file_path, size, 1);
        
        // Swap sinks safely
        // Note: spdlog::logger::sinks() returns a reference to the vector of sinks.
        // We replace the existing sinks with the new file sink.
        auto& sinks = logger_->sinks();
        sinks.clear();
        sinks.push_back(file_sink);
        
        // Force flush on error? Optional configuration
        logger_->flush_on(spdlog::level::err);
    }
}
