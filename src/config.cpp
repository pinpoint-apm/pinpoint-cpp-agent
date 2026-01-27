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

#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <random>
#include <string>
#include <sstream>
#include <algorithm>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"

#include "logging.h"
#include "sampling.h"
#include "utility.h"
#include "config.h"

namespace pinpoint {

    namespace {
        constexpr int kMaxAppNameLength = 24;
        constexpr int kMaxAgentIdLength = 24;
        constexpr int kMaxAgentNameLength = 255;
    }

    static std::string& global_agent_config_str() {
        static std::string cfg_str;
        return cfg_str;
    }

    static bool get_boolean(const YAML::Node& yaml, std::string_view cname, bool default_value) {
        if (yaml[cname]) {
            try {
                return yaml[cname].as<bool>();
            } catch (const YAML::TypedBadConversion<bool>& e) {
                LOG_WARN("Failed to convert '{}' to boolean: {}. Using default value: {}", 
                         std::string(cname), e.what(), default_value);
                return default_value;
            }
        }

        return default_value;
    }

    static std::string get_string(const YAML::Node& yaml, std::string_view cname, std::string default_value) {
        if (yaml[cname]) {
            try {
                return yaml[cname].as<std::string>();
            } catch (const YAML::TypedBadConversion<std::string>& e) {
                LOG_WARN("Failed to convert '{}' to string: {}. Using default value: '{}'", 
                         std::string(cname), e.what(), default_value);
                return default_value;
            }
        }

        return default_value;
    }

    static std::vector<std::string> get_string_vector(const YAML::Node& yaml, std::string_view cname,
                                                      std::vector<std::string> default_value) {
        if (yaml[cname]) {
            try {
                return yaml[cname].as<std::vector<std::string>>();
            } catch (const YAML::TypedBadConversion<std::vector<std::string>>& e) {
                LOG_WARN("Failed to convert '{}' to string vector: {}. Using default value", 
                         std::string(cname), e.what());
                return default_value;
            }
        }

        return default_value;
    }

    static int get_int(const YAML::Node& yaml, std::string_view cname, int default_value) {
        if (yaml[cname]) {
            try {
                return yaml[cname].as<int>();
            } catch (const YAML::TypedBadConversion<int>& e) {
                LOG_WARN("Failed to convert '{}' to int: {}. Using default value: {}", 
                         std::string(cname), e.what(), default_value);
                return default_value;
            }
        }

        return default_value;
    }

    static double get_double(const YAML::Node& yaml, std::string_view cname, double default_value) {
        if (yaml[cname]) {
            try {
                return yaml[cname].as<double>();
            } catch (const YAML::TypedBadConversion<double>& e) {
                LOG_WARN("Failed to convert '{}' to double: {}. Using default value: {}", 
                         std::string(cname), e.what(), default_value);
                return default_value;
            }
        }

        return default_value;
    }

    static void load_yaml_config(const YAML::Node& yaml, Config& config, bool& is_container_set) {
        if (yaml.size() < 1) {
            return;
        }

        config.log.level = get_string(yaml, "LogLevel", defaults::LOG_LEVEL);
        config.enable = get_boolean(yaml, "Enable", true);
        config.app_name_ = get_string(yaml, "ApplicationName", "");
        config.app_type_ = get_int(yaml, "ApplicationType", defaults::APP_TYPE);
        config.agent_id_ = get_string(yaml, "AgentId", "");
        config.agent_name_ = get_string(yaml, "AgentName", "");

        if (auto& log = yaml["Log"]) {
            config.log.level = get_string(log, "Level", defaults::LOG_LEVEL);
            config.log.file_path = get_string(log, "FilePath", "");
            config.log.max_file_size = get_int(log, "MaxFileSize", defaults::LOG_MAX_FILE_SIZE_MB);
        }

        if (auto& collector = yaml["Collector"]) {
            config.collector.host = get_string(collector, "GrpcHost", "");
            config.collector.agent_port = get_int(collector, "GrpcAgentPort", defaults::AGENT_PORT);
            config.collector.span_port = get_int(collector, "GrpcSpanPort", defaults::SPAN_PORT);
            config.collector.stat_port = get_int(collector, "GrpcStatPort", defaults::STAT_PORT);
        }

        if (auto& stat = yaml["Stat"]) {
            config.stat.enable = get_boolean(stat, "Enable", true);
            config.stat.batch_count = get_int(stat, "BatchCount", defaults::STAT_BATCH_COUNT);
            config.stat.collect_interval = get_int(stat, "BatchInterval", defaults::STAT_INTERVAL_MS);
        }

        if (auto& http = yaml["Http"]) {
            config.http.url_stat.enable = get_boolean(http, "CollectUrlStat", false);
            config.http.url_stat.limit = get_int(http, "UrlStatLimit", defaults::HTTP_URL_STAT_LIMIT);
            config.http.url_stat.enable_trim_path = get_boolean(http, "UrlStatEnableTrimPath", true);
            config.http.url_stat.trim_path_depth = get_int(http, "UrlStatTrimPathDepth", 1);
            config.http.url_stat.method_prefix = get_boolean(http, "UrlStatMethodPrefix", false);

            if (auto& srv = http["Server"]) {
                config.http.server.status_errors = get_string_vector(srv, "StatusCodeErrors", {"5xx"});
                config.http.server.exclude_url = get_string_vector(srv, "ExcludeUrl", {});
                config.http.server.exclude_method = get_string_vector(srv, "ExcludeMethod", {});
                config.http.server.rec_request_header = get_string_vector(srv, "RecordRequestHeader", {});
                config.http.server.rec_request_cookie = get_string_vector(srv, "RecordRequestCookie", {});
                config.http.server.rec_response_header = get_string_vector(srv, "RecordResponseHeader", {});
            }

            if (auto& cli = http["Client"]) {
                config.http.client.rec_request_header = get_string_vector(cli, "RecordRequestHeader", {});
                config.http.client.rec_request_cookie = get_string_vector(cli, "RecordRequestCookie", {});
                config.http.client.rec_response_header = get_string_vector(cli, "RecordResponseHeader", {});
            }
        }

        if (auto& sampling = yaml["Sampling"]) {
            config.sampling.type = get_string(sampling, "Type", COUNTER_SAMPLING);
            config.sampling.counter_rate = get_int(sampling, "CounterRate", defaults::SAMPLING_COUNTER_RATE);
            config.sampling.percent_rate = get_double(sampling, "PercentRate", defaults::SAMPLING_PERCENT_RATE);
            config.sampling.new_throughput = get_int(sampling, "NewThroughput", 0);
            config.sampling.cont_throughput = get_int(sampling, "ContinueThroughput", 0);
        }

        if (auto& span = yaml["Span"]) {
            config.span.queue_size = get_int(span, "QueueSize", defaults::SPAN_QUEUE_SIZE);
            config.span.max_event_depth = get_int(span, "MaxEventDepth", defaults::SPAN_MAX_EVENT_DEPTH);
            config.span.max_event_sequence = get_int(span, "MaxEventSequence", defaults::SPAN_MAX_EVENT_SEQUENCE);
            config.span.event_chunk_size = get_int(span, "EventChunkSize", defaults::SPAN_EVENT_CHUNK_SIZE);
        }

        if (yaml["IsContainer"]) {
            config.is_container = get_boolean(yaml, "IsContainer", false);
            is_container_set = true;
        }

        if (auto& sql = yaml["Sql"]) {
            config.sql.max_bind_args_size = get_int(sql, "MaxBindArgsSize", defaults::SQL_MAX_BIND_ARGS_SIZE);
            config.sql.enable_sql_stats = get_boolean(sql, "EnableSqlStats", false);
        }

        config.enable_callstack_trace = get_boolean(yaml, "EnableCallstackTrace", false);
    }

    static bool safe_env_stob(const char* env_name, const char* env_value, bool default_value) {
        auto result = stob_(env_value);
        if (result.has_value()) {
            return result.value();
        } else {
            LOG_WARN("Failed to parse boolean value '{}' for environment variable '{}'. Using default value: {}", 
                     env_value, env_name, default_value);
            return default_value;
        }
    }

    static int safe_env_stoi(const char* env_name, const char* env_value, int default_value) {
        auto result = stoi_(env_value);
        if (result.has_value()) {
            return result.value();
        } else {
            LOG_WARN("Invalid integer value '{}' for environment variable '{}'. Using default value: {}", 
                     env_value, env_name, default_value);
            return default_value;
        }
    }

    static double safe_env_stod(const char* env_name, const char* env_value, double default_value) {
        auto result = stod_(env_value);
        if (result.has_value()) {
            return result.value();
        } else {
            LOG_WARN("Invalid double value '{}' for environment variable '{}'. Using default value: {}", 
                     env_value, env_name, default_value);
            return default_value;
        }
    }

    static void load_env_config(Config& config, bool& is_container_set) {
        if(const char* env_p = std::getenv(env::ENABLE)) {
            config.enable = safe_env_stob(env::ENABLE, env_p, true);
        }
        if(const char* env_p = std::getenv(env::APPLICATION_NAME)) {
            config.app_name_ = std::string(env_p);
        }
        if(const char* env_p = std::getenv(env::APPLICATION_TYPE)) {
            config.app_type_ = safe_env_stoi(env::APPLICATION_TYPE, env_p, defaults::APP_TYPE);
        }
        if(const char* env_p = std::getenv(env::AGENT_ID)) {
            config.agent_id_ = std::string(env_p);
        }
        if(const char* env_p = std::getenv(env::AGENT_NAME)) {
            config.agent_name_ = std::string(env_p);
        }

        if(const char* env_p = std::getenv(env::LOG_LEVEL)) {
            config.log.level = std::string(env_p);
        }
        if(const char* env_p = std::getenv(env::LOG_FILE_PATH)) {
            config.log.file_path = std::string(env_p);
        }
        if(const char* env_p = std::getenv(env::LOG_MAX_FILE_SIZE)) {
            config.log.max_file_size = safe_env_stoi(env::LOG_MAX_FILE_SIZE, env_p, defaults::LOG_MAX_FILE_SIZE_MB);
        }

        if(const char* env_p = std::getenv(env::GRPC_HOST)) {
            config.collector.host = std::string(env_p);
        }
        if(const char* env_p = std::getenv(env::GRPC_AGENT_PORT)) {
            config.collector.agent_port = safe_env_stoi(env::GRPC_AGENT_PORT, env_p, defaults::AGENT_PORT);
        }
        if(const char* env_p = std::getenv(env::GRPC_SPAN_PORT)) {
            config.collector.span_port = safe_env_stoi(env::GRPC_SPAN_PORT, env_p, defaults::SPAN_PORT);
        }
        if(const char* env_p = std::getenv(env::GRPC_STAT_PORT)) {
            config.collector.stat_port = safe_env_stoi(env::GRPC_STAT_PORT, env_p, defaults::STAT_PORT);
        }

        if(const char* env_p = std::getenv(env::STAT_ENABLE)) {
            config.stat.enable = safe_env_stob(env::STAT_ENABLE, env_p, true);
        }
        if(const char* env_p = std::getenv(env::STAT_BATCH_COUNT)) {
            config.stat.batch_count = safe_env_stoi(env::STAT_BATCH_COUNT, env_p, defaults::STAT_BATCH_COUNT);
        }
        if(const char* env_p = std::getenv(env::STAT_BATCH_INTERVAL)) {
            config.stat.collect_interval = safe_env_stoi(env::STAT_BATCH_INTERVAL, env_p, defaults::STAT_INTERVAL_MS);
        }

        if(const char* env_p = std::getenv(env::SAMPLING_TYPE)) {
            config.sampling.type = std::string(env_p);
        }
        if(const char* env_p = std::getenv(env::SAMPLING_COUNTER_RATE)) {
            config.sampling.counter_rate = safe_env_stoi(env::SAMPLING_COUNTER_RATE, env_p, defaults::SAMPLING_COUNTER_RATE);
        }
        if(const char* env_p = std::getenv(env::SAMPLING_PERCENT_RATE)) {
            config.sampling.percent_rate = safe_env_stod(env::SAMPLING_PERCENT_RATE, env_p, defaults::SAMPLING_PERCENT_RATE);
        }
        if(const char* env_p = std::getenv(env::SAMPLING_NEW_THROUGHPUT)) {
            config.sampling.new_throughput = safe_env_stoi(env::SAMPLING_NEW_THROUGHPUT, env_p, 0);
        }
        if(const char* env_p = std::getenv(env::SAMPLING_CONTINUE_THROUGHPUT)) {
            config.sampling.cont_throughput = safe_env_stoi(env::SAMPLING_CONTINUE_THROUGHPUT, env_p, 0);
        }

        if(const char* env_p = std::getenv(env::SPAN_QUEUE_SIZE)) {
            config.span.queue_size = safe_env_stoi(env::SPAN_QUEUE_SIZE, env_p, defaults::SPAN_QUEUE_SIZE);
        }
        if(const char* env_p = std::getenv(env::SPAN_MAX_EVENT_DEPTH)) {
            config.span.max_event_depth = safe_env_stoi(env::SPAN_MAX_EVENT_DEPTH, env_p, defaults::SPAN_MAX_EVENT_DEPTH);
        }
        if(const char* env_p = std::getenv(env::SPAN_MAX_EVENT_SEQUENCE)) {
            config.span.max_event_sequence = safe_env_stoi(env::SPAN_MAX_EVENT_SEQUENCE, env_p, defaults::SPAN_MAX_EVENT_SEQUENCE);
        }
        if(const char* env_p = std::getenv(env::SPAN_EVENT_CHUNK_SIZE)) {
            config.span.event_chunk_size = safe_env_stoi(env::SPAN_EVENT_CHUNK_SIZE, env_p, defaults::SPAN_EVENT_CHUNK_SIZE);
        }

        if(const char* env_p = std::getenv(env::IS_CONTAINER)) {
            config.is_container = safe_env_stob(env::IS_CONTAINER, env_p, false);
            is_container_set = true;
        }

        if(const char* env_p = std::getenv(env::HTTP_COLLECT_URL_STAT)) {
            config.http.url_stat.enable = safe_env_stob(env::HTTP_COLLECT_URL_STAT, env_p, false);
        }
        if(const char* env_p = std::getenv(env::HTTP_URL_STAT_LIMIT)) {
            config.http.url_stat.limit = safe_env_stoi(env::HTTP_URL_STAT_LIMIT, env_p, defaults::HTTP_URL_STAT_LIMIT);
        }
        if(const char* env_p = std::getenv(env::HTTP_URL_STAT_ENABLE_TRIM_PATH)) {
            config.http.url_stat.enable_trim_path = safe_env_stob(env::HTTP_URL_STAT_ENABLE_TRIM_PATH, env_p, true);
        }
        if(const char* env_p = std::getenv(env::HTTP_URL_STAT_TRIM_PATH_DEPTH)) {
            config.http.url_stat.trim_path_depth = safe_env_stoi(env::HTTP_URL_STAT_TRIM_PATH_DEPTH, env_p, 1);
        }
        if(const char* env_p = std::getenv(env::HTTP_URL_STAT_METHOD_PREFIX)) {
            config.http.url_stat.method_prefix = safe_env_stob(env::HTTP_URL_STAT_METHOD_PREFIX, env_p, false);
        }

        if(const char* env_p = std::getenv(env::HTTP_SERVER_STATUS_CODE_ERRORS)) {
            config.http.server.status_errors = absl::StrSplit(env_p, ',');
        }
        if(const char* env_p = std::getenv(env::HTTP_SERVER_EXCLUDE_URL)) {
            config.http.server.exclude_url = absl::StrSplit(env_p, ',');
        }
        if(const char* env_p = std::getenv(env::HTTP_SERVER_EXCLUDE_METHOD)) {
            config.http.server.exclude_method = absl::StrSplit(env_p, ',');
        }
        if(const char* env_p = std::getenv(env::HTTP_SERVER_RECORD_REQUEST_HEADER)) {
            config.http.server.rec_request_header = absl::StrSplit(env_p, ',');
        }
        if(const char* env_p = std::getenv(env::HTTP_SERVER_RECORD_REQUEST_COOKIE)) {
            config.http.server.rec_request_cookie = absl::StrSplit(env_p, ',');
        }
        if(const char* env_p = std::getenv(env::HTTP_SERVER_RECORD_RESPONSE_HEADER)) {
            config.http.server.rec_response_header = absl::StrSplit(env_p, ',');
        }
        if(const char* env_p = std::getenv(env::HTTP_CLIENT_RECORD_REQUEST_HEADER)) {
            config.http.client.rec_request_header = absl::StrSplit(env_p, ',');
        }
        if(const char* env_p = std::getenv(env::HTTP_CLIENT_RECORD_REQUEST_COOKIE)) {
            config.http.client.rec_request_cookie = absl::StrSplit(env_p, ',');
        }
        if(const char* env_p = std::getenv(env::HTTP_CLIENT_RECORD_RESPONSE_HEADER)) {
            config.http.client.rec_response_header = absl::StrSplit(env_p, ',');
        }

        if(const char* env_p = std::getenv(env::SQL_MAX_BIND_ARGS_SIZE)) {
            config.sql.max_bind_args_size = safe_env_stoi(env::SQL_MAX_BIND_ARGS_SIZE, env_p, defaults::SQL_MAX_BIND_ARGS_SIZE);
        }
        if(const char* env_p = std::getenv(env::SQL_ENABLE_SQL_STATS)) {
            config.sql.enable_sql_stats = safe_env_stob(env::SQL_ENABLE_SQL_STATS, env_p, false);
        }
        if(const char* env_p = std::getenv(env::ENABLE_CALLSTACK_TRACE)) {
            config.enable_callstack_trace = safe_env_stob(env::ENABLE_CALLSTACK_TRACE, env_p, false);
        }
    }

    static bool is_container_env() {
        FILE* f = fopen("/.dockerenv", "r");
        if (f != nullptr) {
            fclose(f);
            return true;
        }

        const char *tmp = std::getenv("KUBERNETES_SERVICE_HOST");
        std::string env_var(tmp ? tmp : "");
        if (!env_var.empty()) {
            return true;
        }

        return false;
    }

    static const int kAgentIdPrefixLength = 18;
    static const int kAgentIdRandomLength = 5;

    static std::string randomString()
    {
        const std::string chars = "0123456789abcdefghijklmnopqrstuvwxyz";

        std::mt19937 gen(std::random_device{}());
        std::uniform_int_distribution<> dist(0, (int)chars.size() - 1);

        auto rand_char = [chars, &gen, &dist]() { return chars[dist(gen)]; };

        std::string random_string(kAgentIdRandomLength,0);
        std::generate_n(random_string.begin(), kAgentIdRandomLength, rand_char);

        return random_string;
    }

    static std::string generate_agent_id() {
        auto hostname = get_host_name();

        if (hostname.length() > kAgentIdPrefixLength) {
            hostname = hostname.substr(0, kAgentIdPrefixLength);
        }

        return absl::StrCat(hostname, "-", randomString());
    }

    void read_config_from_file(const char* config_file_path) {
        if (std::ifstream file(config_file_path); file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            global_agent_config_str() = buffer.str();
            file.close();
        } else {
            LOG_ERROR("can't open config file = {}", config_file_path);
        }
    }

    void set_config_string(std::string_view cfg_str) {
        global_agent_config_str() = cfg_str;
    }

    constexpr int NONE_SAMPLING_COUNTER_RATE = 0;
    constexpr double NONE_SAMPLING_PERCENT_RATE = 0.0;
    constexpr int NONE_SAMPLING_NEW_THROUGHPUT = 0;
    constexpr int NONE_SAMPLING_CONTINUE_THROUGHPUT = 0;
    constexpr double MIN_SAMPLING_PERCENT_RATE = 0.01;
    constexpr double MAX_SAMPLING_PERCENT_RATE = 100.0;
    constexpr int MIN_SPAN_QUEUE_SIZE = 1;
    constexpr int UNLIMITED_SIZE = -1;
    constexpr int MIN_SPAN_EVENT_DEPTH = 2;
    constexpr int MIN_SPAN_EVENT_SEQUENCE = 4;
    constexpr int MIN_SPAN_EVENT_CHUNK_SIZE = 1;
    constexpr int MAX_SPAN_EVENT_DEPTH = INT32_MAX;
    constexpr int MAX_SPAN_EVENT_SEQUENCE = INT32_MAX;

    Config make_config() {
        Config config;
        bool is_container_set = false;

        init_logger();

        if(const char* env_p = std::getenv(env::CONFIG_FILE); env_p != nullptr) {
            read_config_from_file(env_p);
        }

        YAML::Node yaml;
        const auto& user_config = global_agent_config_str();
        if (!user_config.empty()) {
            try {
                yaml = YAML::Load(user_config);
            } catch (const YAML::ParserException& e) {
                LOG_ERROR("yaml parsing exception = {}", e.what());
                return config;
            }
        }

        load_yaml_config(yaml, config, is_container_set);
        load_env_config(config, is_container_set);

        if (!config.log.file_path.empty()) {
            Logger::getInstance().setFileLogger(config.log.file_path, config.log.max_file_size);
        }
        Logger::getInstance().setLogLevel(config.log.level);

        if (config.agent_id_.empty()) {
            config.agent_id_ = generate_agent_id();
        }

        if (config.sampling.counter_rate < NONE_SAMPLING_COUNTER_RATE) {
            config.sampling.counter_rate = NONE_SAMPLING_COUNTER_RATE;
        }
        if (config.sampling.percent_rate < NONE_SAMPLING_PERCENT_RATE) {
            config.sampling.percent_rate = NONE_SAMPLING_PERCENT_RATE;
        } else if (config.sampling.percent_rate < MIN_SAMPLING_PERCENT_RATE) {
            config.sampling.percent_rate = MIN_SAMPLING_PERCENT_RATE;
        } else if (config.sampling.percent_rate > MAX_SAMPLING_PERCENT_RATE) {
            config.sampling.percent_rate = MAX_SAMPLING_PERCENT_RATE;
        }
        if (config.sampling.new_throughput < NONE_SAMPLING_NEW_THROUGHPUT) {
            config.sampling.new_throughput = NONE_SAMPLING_NEW_THROUGHPUT;
        }
        if (config.sampling.cont_throughput < NONE_SAMPLING_CONTINUE_THROUGHPUT) {
            config.sampling.cont_throughput = NONE_SAMPLING_CONTINUE_THROUGHPUT;
        }

        if (config.span.queue_size < MIN_SPAN_QUEUE_SIZE) {
            config.span.queue_size = defaults::SPAN_QUEUE_SIZE;
        }
        if (config.span.max_event_depth == UNLIMITED_SIZE) {
            config.span.max_event_depth = MAX_SPAN_EVENT_DEPTH;
        } else if (config.span.max_event_depth < MIN_SPAN_EVENT_DEPTH) {
            config.span.max_event_depth = MIN_SPAN_EVENT_DEPTH;
        }
        if (config.span.max_event_sequence == UNLIMITED_SIZE) {
            config.span.max_event_sequence = MAX_SPAN_EVENT_SEQUENCE;
        } else if (config.span.max_event_sequence < MIN_SPAN_EVENT_SEQUENCE) {
            config.span.max_event_sequence = MIN_SPAN_EVENT_SEQUENCE;
        }
        if (config.span.event_chunk_size < MIN_SPAN_EVENT_CHUNK_SIZE) {
            config.span.event_chunk_size = defaults::SPAN_EVENT_CHUNK_SIZE;
        }

        if (!is_container_set) {
            config.is_container = is_container_env();
        }

        LOG_INFO("config: {}", "\n" + to_config_string(config));
        return config;
    }

    std::string to_config_string(const Config& config) {
        YAML::Emitter emitter;

        emitter << YAML::BeginMap;
        emitter << YAML::Key << "ApplicationName" << YAML::Value << config.app_name_;
        emitter << YAML::Key << "ApplicationType" << YAML::Value << config.app_type_;
        emitter << YAML::Key << "AgentId" << YAML::Value << config.agent_id_;
        emitter << YAML::Key << "AgentName" << YAML::Value << config.agent_name_;

        emitter << YAML::Key << "Enable" << YAML::Value << config.enable;
        emitter << YAML::Key << "IsContainer" << YAML::Value << config.is_container;

        emitter << YAML::Key << "Log";
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "Level" << YAML::Value << config.log.level;
        emitter << YAML::Key << "FilePath" << YAML::Value << config.log.file_path;
        emitter << YAML::Key << "MaxFileSize" << YAML::Value << config.log.max_file_size;
        emitter << YAML::EndMap;

        emitter << YAML::Key << "Collector";
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "GrpcHost" << YAML::Value << config.collector.host;
        emitter << YAML::Key << "GrpcAgentPort" << YAML::Value << config.collector.agent_port;
        emitter << YAML::Key << "GrpcSpanPort" << YAML::Value << config.collector.span_port;
        emitter << YAML::Key << "GrpcStatPort" << YAML::Value << config.collector.stat_port;
        emitter << YAML::EndMap;

        emitter << YAML::Key << "Stat";
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "Enable" << YAML::Value << config.stat.enable;
        emitter << YAML::Key << "BatchCount" << YAML::Value << config.stat.batch_count;
        emitter << YAML::Key << "BatchInterval" << YAML::Value << config.stat.collect_interval;
        emitter << YAML::EndMap;

        emitter << YAML::Key << "Sampling";
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "Type" << YAML::Value << config.sampling.type;
        emitter << YAML::Key << "CounterRate" << YAML::Value << config.sampling.counter_rate;
        emitter << YAML::Key << "PercentRate" << YAML::Value << config.sampling.percent_rate;
        emitter << YAML::Key << "NewThroughput" << YAML::Value << config.sampling.new_throughput;
        emitter << YAML::Key << "ContThroughput" << YAML::Value << config.sampling.cont_throughput;
        emitter << YAML::EndMap;

        emitter << YAML::Key << "Span";
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "QueueSize" << YAML::Value << config.span.queue_size;
        emitter << YAML::Key << "MaxEventDepth" << YAML::Value << config.span.max_event_depth;
        emitter << YAML::Key << "MaxEventSequence" << YAML::Value << config.span.max_event_sequence;
        emitter << YAML::Key << "EventChunkSize" << YAML::Value << config.span.event_chunk_size;
        emitter << YAML::EndMap;

        emitter << YAML::Key << "Http";
        emitter << YAML::BeginMap;

        emitter << YAML::Key << "UrlStat";
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "Enable" << YAML::Value << config.http.url_stat.enable;
        emitter << YAML::Key << "Limit" << YAML::Value << config.http.url_stat.limit;
        emitter << YAML::Key << "PathDepth" << YAML::Value << config.http.url_stat.trim_path_depth;
        emitter << YAML::Key << "MethodPrefix" << YAML::Value << config.http.url_stat.method_prefix;
        emitter << YAML::EndMap;

        emitter << YAML::Key << "Server";
        emitter << YAML::BeginMap;

        emitter << YAML::Key << "StatusCodeErrors" << YAML::Value << YAML::BeginSeq;
        for (const auto& s : config.http.server.status_errors) {
            emitter << s;
        }
        emitter << YAML::EndSeq;

        emitter << YAML::Key << "ExcludeUrl" << YAML::Value << YAML::BeginSeq;
        for (const auto& s : config.http.server.exclude_url) {
            emitter << s;
        }
        emitter << YAML::EndSeq;

        emitter << YAML::Key << "ExcludeMethod" << YAML::Value << YAML::BeginSeq;
        for (const auto& s : config.http.server.exclude_method) {
            emitter << s;
        }
        emitter << YAML::EndSeq;

        emitter << YAML::Key << "RecordRequestHeader" << YAML::Value << YAML::BeginSeq;
        for (const auto& s : config.http.server.rec_request_header) {
            emitter << s;
        }
        emitter << YAML::EndSeq;

        emitter << YAML::Key << "RecordRequestCookie" << YAML::Value << YAML::BeginSeq;
        for (const auto& s : config.http.server.rec_request_cookie) {
            emitter << s;
        }
        emitter << YAML::EndSeq;

        emitter << YAML::Key << "RecordResponseHeader" << YAML::Value << YAML::BeginSeq;
        for (const auto& s : config.http.server.rec_response_header) {
            emitter << s;
        }
        emitter << YAML::EndSeq;
        emitter << YAML::EndMap;

        emitter << YAML::Key << "Client";
        emitter << YAML::BeginMap;

        emitter << YAML::Key << "RecordRequestHeader" << YAML::Value << YAML::BeginSeq;
        for (const auto& s : config.http.client.rec_request_header) {
            emitter << s;
        }
        emitter << YAML::EndSeq;

        emitter << YAML::Key << "RecordRequestCookie" << YAML::Value << YAML::BeginSeq;
        for (const auto& s : config.http.client.rec_request_cookie) {
            emitter << s;
        }
        emitter << YAML::EndSeq;

        emitter << YAML::Key << "RecordResponseHeader" << YAML::Value << YAML::BeginSeq;
        for (const auto& s : config.http.client.rec_response_header) {
            emitter << s;
        }
        emitter << YAML::EndSeq;

        emitter << YAML::EndMap;
        emitter << YAML::EndMap;

        emitter << YAML::Key << "Sql";
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "MaxBindArgsSize" << YAML::Value << config.sql.max_bind_args_size;
        emitter << YAML::Key << "EnableSqlStats" << YAML::Value << config.sql.enable_sql_stats;
        emitter << YAML::EndMap;

        emitter << YAML::Key << "EnableCallstackTrace" << YAML::Value << config.enable_callstack_trace;
        emitter << YAML::EndMap;

        return emitter.c_str();
    }

    bool Config::check() const {
        if (collector.host.empty()) {
            LOG_ERROR("address of collector is required");
            return false;
        }
        if (app_name_.empty()) {
            LOG_ERROR("application name is required");
            return false;
        }
        if (app_name_.size() > kMaxAppNameLength) {
            LOG_ERROR("application name is too long - max length: {}", kMaxAppNameLength);
            return false;
        }
        if (agent_id_.size() > kMaxAgentIdLength) {
            LOG_ERROR("agent id is too long - max length: {}", kMaxAgentIdLength);
            return false;
        }
        if (agent_name_.size() > kMaxAgentNameLength) {
            LOG_ERROR("agent name is too long - max length: {}", kMaxAgentNameLength);
            return false;
        }

        return true;
    }

    bool Config::isReloadable(const Config& old) {
        const bool identity_same =
            app_name_ == old.app_name_ &&
            app_type_ == old.app_type_ &&
            agent_id_ == old.agent_id_ &&
            agent_name_ == old.agent_name_ &&
            collector.host == old.collector.host &&
            collector.agent_port == old.collector.agent_port &&
            collector.span_port == old.collector.span_port &&
            collector.stat_port == old.collector.stat_port;

        // When identity/collector settings are the same, do not reload.
        return !identity_same;
    }
}
