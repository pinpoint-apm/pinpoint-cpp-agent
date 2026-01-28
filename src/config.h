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
#include <yaml-cpp/yaml.h>
#include "pinpoint/tracer.h"
#include "sampling.h"

namespace pinpoint {

    namespace defaults {
        constexpr int AGENT_PORT = 9991;
        constexpr int SPAN_PORT = 9993;
        constexpr int STAT_PORT = 9992;
        constexpr int STAT_BATCH_COUNT = 6;
        constexpr int STAT_INTERVAL_MS = 5000;
        constexpr int SAMPLING_COUNTER_RATE = 1;
        constexpr double SAMPLING_PERCENT_RATE = 100.0;
        constexpr int SPAN_QUEUE_SIZE = 1024;
        constexpr int SPAN_MAX_EVENT_DEPTH = 64;
        constexpr int SPAN_MAX_EVENT_SEQUENCE = 5000;
        constexpr int SPAN_EVENT_CHUNK_SIZE = 20;
        constexpr int HTTP_URL_STAT_LIMIT = 1024;
        constexpr int SQL_MAX_BIND_ARGS_SIZE = 1024;
        constexpr int LOG_MAX_FILE_SIZE_MB = 10;
        constexpr const char* LOG_LEVEL = "info";

        constexpr int32_t APP_TYPE = APP_TYPE_CPP;
        constexpr int32_t SPAN_SERVICE_TYPE = SERVICE_TYPE_CPP;
        constexpr int32_t SPAN_EVENT_SERVICE_TYPE = SERVICE_TYPE_CPP_FUNC;
    }

    /**
     * @brief Environment variable names that override configuration values.
     */
    namespace env {
        constexpr const char* ENABLE = "PINPOINT_CPP_ENABLE";
        constexpr const char* APPLICATION_NAME = "PINPOINT_CPP_APPLICATION_NAME";
        constexpr const char* APPLICATION_TYPE = "PINPOINT_CPP_APPLICATION_TYPE";
        constexpr const char* AGENT_ID = "PINPOINT_CPP_AGENT_ID";
        constexpr const char* AGENT_NAME = "PINPOINT_CPP_AGENT_NAME";
        constexpr const char* LOG_LEVEL = "PINPOINT_CPP_LOG_LEVEL";
        constexpr const char* LOG_FILE_PATH = "PINPOINT_CPP_LOG_FILE_PATH";
        constexpr const char* LOG_MAX_FILE_SIZE = "PINPOINT_CPP_LOG_MAX_FILE_SIZE";
        constexpr const char* GRPC_HOST = "PINPOINT_CPP_GRPC_HOST";
        constexpr const char* GRPC_AGENT_PORT = "PINPOINT_CPP_GRPC_AGENT_PORT";
        constexpr const char* GRPC_SPAN_PORT = "PINPOINT_CPP_GRPC_SPAN_PORT";
        constexpr const char* GRPC_STAT_PORT = "PINPOINT_CPP_GRPC_STAT_PORT";
        constexpr const char* STAT_ENABLE = "PINPOINT_CPP_STAT_ENABLE";
        constexpr const char* STAT_BATCH_COUNT = "PINPOINT_CPP_STAT_BATCH_COUNT";
        constexpr const char* STAT_BATCH_INTERVAL = "PINPOINT_CPP_STAT_BATCH_INTERVAL";
        constexpr const char* SAMPLING_TYPE = "PINPOINT_CPP_SAMPLING_TYPE";
        constexpr const char* SAMPLING_COUNTER_RATE = "PINPOINT_CPP_SAMPLING_COUNTER_RATE";
        constexpr const char* SAMPLING_PERCENT_RATE = "PINPOINT_CPP_SAMPLING_PERCENT_RATE";
        constexpr const char* SAMPLING_NEW_THROUGHPUT = "PINPOINT_CPP_SAMPLING_NEW_THROUGHPUT";
        constexpr const char* SAMPLING_CONTINUE_THROUGHPUT = "PINPOINT_CPP_SAMPLING_CONTINUE_THROUGHPUT";
        constexpr const char* SPAN_QUEUE_SIZE = "PINPOINT_CPP_SPAN_QUEUE_SIZE";
        constexpr const char* SPAN_MAX_EVENT_DEPTH = "PINPOINT_CPP_SPAN_MAX_EVENT_DEPTH";
        constexpr const char* SPAN_MAX_EVENT_SEQUENCE = "PINPOINT_CPP_SPAN_MAX_EVENT_SEQUENCE";
        constexpr const char* SPAN_EVENT_CHUNK_SIZE = "PINPOINT_CPP_SPAN_EVENT_CHUNK_SIZE";
        constexpr const char* IS_CONTAINER = "PINPOINT_CPP_IS_CONTAINER";
        constexpr const char* HTTP_COLLECT_URL_STAT = "PINPOINT_CPP_HTTP_COLLECT_URL_STAT";
        constexpr const char* HTTP_URL_STAT_LIMIT = "PINPOINT_CPP_HTTP_URL_STAT_LIMIT";
        constexpr const char* HTTP_URL_STAT_ENABLE_TRIM_PATH = "PINPOINT_CPP_HTTP_URL_STAT_ENABLE_TRIM_PATH";
        constexpr const char* HTTP_URL_STAT_TRIM_PATH_DEPTH = "PINPOINT_CPP_HTTP_URL_STAT_TRIM_PATH_DEPTH";
        constexpr const char* HTTP_URL_STAT_METHOD_PREFIX = "PINPOINT_CPP_HTTP_URL_STAT_METHOD_PREFIX";
        constexpr const char* HTTP_SERVER_STATUS_CODE_ERRORS = "PINPOINT_CPP_HTTP_SERVER_STATUS_CODE_ERRORS";
        constexpr const char* HTTP_SERVER_EXCLUDE_URL = "PINPOINT_CPP_HTTP_SERVER_EXCLUDE_URL";
        constexpr const char* HTTP_SERVER_EXCLUDE_METHOD = "PINPOINT_CPP_HTTP_SERVER_EXCLUDE_METHOD";
        constexpr const char* HTTP_SERVER_RECORD_REQUEST_HEADER = "PINPOINT_CPP_HTTP_SERVER_RECORD_REQUEST_HEADER";
        constexpr const char* HTTP_SERVER_RECORD_REQUEST_COOKIE = "PINPOINT_CPP_HTTP_SERVER_RECORD_REQUEST_COOKIE";
        constexpr const char* HTTP_SERVER_RECORD_RESPONSE_HEADER = "PINPOINT_CPP_HTTP_SERVER_RECORD_RESPONSE_HEADER";
        constexpr const char* HTTP_CLIENT_RECORD_REQUEST_HEADER = "PINPOINT_CPP_HTTP_CLIENT_RECORD_REQUEST_HEADER";
        constexpr const char* HTTP_CLIENT_RECORD_REQUEST_COOKIE = "PINPOINT_CPP_HTTP_CLIENT_RECORD_REQUEST_COOKIE";
        constexpr const char* HTTP_CLIENT_RECORD_RESPONSE_HEADER = "PINPOINT_CPP_HTTP_CLIENT_RECORD_RESPONSE_HEADER";
        constexpr const char* SQL_MAX_BIND_ARGS_SIZE = "PINPOINT_CPP_SQL_MAX_BIND_ARGS_SIZE";
        constexpr const char* SQL_ENABLE_SQL_STATS = "PINPOINT_CPP_SQL_ENABLE_SQL_STATS";
        constexpr const char* CONFIG_FILE = "PINPOINT_CPP_CONFIG_FILE";
        constexpr const char* ENABLE_CALLSTACK_TRACE = "PINPOINT_CPP_ENABLE_CALLSTACK_TRACE";
    }

    /**
     * @brief Aggregated runtime configuration used by the Pinpoint agent.
     *
     * The structure mirrors values available in the YAML configuration file and environment
     * overrides. Nested structures keep related options together for readability.
     */
    struct Config {
        std::string app_name_;
        int32_t app_type_ = defaults::APP_TYPE;
        std::string agent_id_;
        std::string agent_name_;

        bool enable = true;
        bool is_container = false;
        bool enable_callstack_trace = false;

        struct {
            std::string level = defaults::LOG_LEVEL;
            std::string file_path;
            int max_file_size = defaults::LOG_MAX_FILE_SIZE_MB;
        } log;

        struct {
            std::string host;
            int agent_port = defaults::AGENT_PORT;
            int span_port = defaults::SPAN_PORT;
            int stat_port = defaults::STAT_PORT;
        } collector;

        struct {
            bool enable = true;
            int batch_count = defaults::STAT_BATCH_COUNT;
            int collect_interval = defaults::STAT_INTERVAL_MS;
        } stat;

        struct {
            std::string type = COUNTER_SAMPLING;
            int counter_rate = defaults::SAMPLING_COUNTER_RATE;
            double percent_rate = defaults::SAMPLING_PERCENT_RATE;
            int new_throughput = 0;
            int cont_throughput = 0;
        } sampling;

        struct {
            size_t queue_size = defaults::SPAN_QUEUE_SIZE;
            int max_event_depth = defaults::SPAN_MAX_EVENT_DEPTH;
            int max_event_sequence = defaults::SPAN_MAX_EVENT_SEQUENCE;
            size_t event_chunk_size = defaults::SPAN_EVENT_CHUNK_SIZE;
        } span;

        struct {
            struct {
                bool enable = false;
                int limit = defaults::HTTP_URL_STAT_LIMIT;
                bool enable_trim_path = true;
                int trim_path_depth = 1;
                bool method_prefix = false;
            } url_stat;

            struct {
                std::vector<std::string> status_errors = {"5xx"};
                std::vector<std::string> exclude_url;
                std::vector<std::string> exclude_method;
                std::vector<std::string> rec_request_header;
                std::vector<std::string> rec_request_cookie;
                std::vector<std::string> rec_response_header;
            } server;

            struct {
                std::vector<std::string> rec_request_header;
                std::vector<std::string> rec_request_cookie;
                std::vector<std::string> rec_response_header;
            } client;
        } http;

        struct {
            int max_bind_args_size = defaults::SQL_MAX_BIND_ARGS_SIZE;
            bool enable_sql_stats = false;
        } sql;

        /**
         * @brief Validates required config fields and constraints.
         *
         * @return true when the configuration is valid.
         */
        bool check() const;

        /**
         * @brief Determines whether a config reload is allowed.
         *
         * @param old Existing config.
         * @return true if reload is allowed.
         */
        bool isReloadable(const std::shared_ptr<const Config>& old) const;
    };

    /**
     * @brief Reads configuration from a YAML file on disk.
     *
     * @param config_file_path Absolute or relative path to the configuration file.
     */
    void read_config_from_file(const char* config_file_path);
    /**
     * @brief Sets the raw YAML configuration source used by `make_config`.
     *
     * @param cfg_str YAML configuration string.
     */
    void set_config_string(std::string_view cfg_str);
    /**
     * @brief Builds a `Config` object by combining defaults, the cached YAML and environment overrides.
     *
     * @return Resolved configuration ready to be consumed by the agent.
     */
    std::shared_ptr<Config> make_config();
    /**
     * @brief Serializes a `Config` object back into its YAML representation.
     *
     * @param config Configuration instance to serialize.
     * @return YAML string.
     */
    std::string to_config_string(const Config& config);
}  // namespace pinpoint
