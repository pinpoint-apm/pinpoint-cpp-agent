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

    class Logger {
    public:
        Logger(const Logger &) = delete;
        Logger &operator=(const Logger &) = delete;

        static Logger& getInstance() {
            // Singleton
            std::call_once(init_flag_, []() {
                instance_.reset(new Logger);
            });
            return *(instance_);
        }

        std::shared_ptr<spdlog::logger>& getLogger() { return logger_; }
        void setLogLevel(const std::string& log_level) const;
        void setFileLogger(const std::string& log_file_path, const int max_size);

    private:
        static std::unique_ptr<Logger> instance_;
        static std::once_flag init_flag_;
        std::shared_ptr<spdlog::logger>	logger_;

        Logger() : logger_(spdlog::stdout_color_mt("pinpoint")) {}
    };

    void init_logger();
    void shutdown_logger();

    #define LOG_DEBUG(...) (Logger::getInstance().getLogger()->debug(__VA_ARGS__))
    #define LOG_INFO(...) (Logger::getInstance().getLogger()->info(__VA_ARGS__))
    #define LOG_WARN(...) (Logger::getInstance().getLogger()->warn(__VA_ARGS__))
    #define LOG_ERROR(...) (Logger::getInstance().getLogger()->error(__VA_ARGS__))
}
