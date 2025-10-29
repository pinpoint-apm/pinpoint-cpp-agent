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

#include <string>
#include <yaml-cpp/yaml.h>

namespace pinpoint {

    // Environment variable names as constexpr
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
        constexpr const char* HTTP_URL_STAT_PATH_DEPTH = "PINPOINT_CPP_HTTP_URL_STAT_PATH_DEPTH";
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

    struct Config {
        std::string app_name_;
        int32_t app_type_;
        std::string agent_id_;
        std::string agent_name_;

        bool enable;
        bool is_container;
        bool enable_callstack_trace;

        struct {
            std::string level;
            std::string file_path;
            int max_file_size;
        } log;

        struct {
            std::string host;
            int agent_port;
            int span_port;
            int stat_port;
        } collector;

        struct {
            bool enable;
            int batch_count;
            int collect_interval;
        } stat;

        struct {
            std::string type;
            int counter_rate;
            double percent_rate;
            int new_throughput;
            int cont_throughput;
        } sampling;

        struct {
            size_t queue_size;
            int max_event_depth;
            int max_event_sequence;
            size_t event_chunk_size;
        } span;

        struct {
            struct {
                bool enable;
                int limit;
                int path_depth;
                bool method_prefix;
            } url_stat;

            struct {
                std::vector<std::string> status_errors;
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
            int max_bind_args_size;
            bool enable_sql_stats;
        } sql;
    };

    void read_config_from_file(const char* config_file_path);
    void set_config_string(std::string_view cfg_str);
    Config make_config();
    std::string to_config_string(const Config& config);
}  // namespace pinpoint
