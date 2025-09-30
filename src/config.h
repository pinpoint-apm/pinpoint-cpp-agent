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

    struct Config {
        std::string app_name_;
        int32_t app_type_;
        std::string agent_id_;
        std::string agent_name_;

        bool enable;
        bool is_container;

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
