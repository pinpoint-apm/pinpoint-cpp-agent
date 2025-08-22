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

#include "logging.h"

namespace pinpoint {
    std::unique_ptr<Logger> Logger::instance_;
    std::once_flag Logger::init_flag_;

    void init_logger() {
        Logger::getInstance().setLogLevel("info");
    }

    void shutdown_logger() {
        spdlog::shutdown();
    }

    void Logger::setLogLevel(const std::string& log_level) const {
        if (const auto level = log_level.c_str(); !strcasecmp(level, "debug")) {
            logger_->set_level(spdlog::level::debug);
        } else if (!strcasecmp(level, "info")) {
            logger_->set_level(spdlog::level::info);
        } else if (!strcasecmp(level, "warning")) {
            logger_->set_level(spdlog::level::warn);
        } else if (!strcasecmp(level, "error")) {
            logger_->set_level(spdlog::level::err);
        }
    }

    void Logger::setFileLogger(const std::string& log_file_path, const int max_size) {
        spdlog::drop("pinpoint");
        const auto size = max_size * 1024 * 1024;
        logger_ = spdlog::rotating_logger_mt("pinpoint", log_file_path, size, 1);
    }
}
