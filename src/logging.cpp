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
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace pinpoint {

    Logger::Logger() {
    }

    void init_logger() {
        Logger::getInstance().setLogLevel(LOG_LEVEL_INFO);
    }

    void shutdown_logger() {
        Logger::getInstance().shutdown();
    }

    void Logger::setLogLevel(const std::string& log_level) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto level = log_level.c_str();
        if (!strcasecmp(level, LOG_LEVEL_DEBUG)) {
            current_level_.store(static_cast<int>(LogLevel::kDebug), std::memory_order_relaxed);
        } else if (!strcasecmp(level, LOG_LEVEL_INFO)) {
            current_level_.store(static_cast<int>(LogLevel::kInfo), std::memory_order_relaxed);
        } else if (!strcasecmp(level, LOG_LEVEL_WARN)) {
            current_level_.store(static_cast<int>(LogLevel::kWarn), std::memory_order_relaxed);
        } else if (!strcasecmp(level, LOG_LEVEL_ERROR)) {
            current_level_.store(static_cast<int>(LogLevel::kError), std::memory_order_relaxed);
        }
    }

    void Logger::setFileLogger(const std::string& log_file_path, const int max_size) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_path_ = log_file_path;
        max_file_size_ = static_cast<std::uint64_t>(max_size) * 1024 * 1024;
        current_file_size_ = 0;
        file_enabled_ = !file_path_.empty();

        file_stream_.reset();
        if (!file_enabled_) {
            return;
        }

        std::error_code ec;
        const auto size = std::filesystem::file_size(file_path_, ec);
        if (!ec) {
            current_file_size_ = static_cast<std::uint64_t>(size);
        }

        file_stream_ = std::make_unique<std::ofstream>(file_path_, std::ios::out | std::ios::app);
        if (!file_stream_->is_open()) {
            file_enabled_ = false;
            file_stream_.reset();
        }
    }

    void Logger::shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_stream_ && file_stream_->is_open()) {
            file_stream_->flush();
            file_stream_->close();
        }
        file_stream_.reset();
        file_enabled_ = false;
    }

    void Logger::write(LogLevel level, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);

        const auto now = std::chrono::system_clock::now();
        const auto now_time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tm{};
        localtime_r(&now_time, &tm);

        std::ostringstream prefix;
        prefix << "[" << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        prefix << "." << std::setw(3) << std::setfill('0') << ms.count() << "]";
        prefix << "[" << (level == LogLevel::kDebug ? LOG_LEVEL_DEBUG :
                          level == LogLevel::kInfo ? LOG_LEVEL_INFO :
                          level == LogLevel::kWarn ? LOG_LEVEL_WARN : LOG_LEVEL_ERROR) << "]";
        prefix << "[pinpoint] ";

        const std::string line = prefix.str() + message + "\n";
        if (file_enabled_ && file_stream_) {
            file_stream_->write(line.data(), static_cast<std::streamsize>(line.size()));
            current_file_size_ += static_cast<std::uint64_t>(line.size());
            rotateFileIfNeededLocked();
        } else {
            std::cout << line;
        }
    }

    void Logger::rotateFileIfNeededLocked() {
        if (!file_enabled_ || !file_stream_ || max_file_size_ == 0) {
            return;
        }
        if (current_file_size_ < max_file_size_) {
            return;
        }

        file_stream_->flush();
        file_stream_->close();

        std::error_code ec;
        const auto rotated_path = file_path_ + ".1";
        std::filesystem::remove(rotated_path, ec);
        std::filesystem::rename(file_path_, rotated_path, ec);

        file_stream_ = std::make_unique<std::ofstream>(file_path_, std::ios::out | std::ios::app);
        current_file_size_ = 0;
        if (!file_stream_->is_open()) {
            file_enabled_ = false;
            file_stream_.reset();
        }
    }
}
