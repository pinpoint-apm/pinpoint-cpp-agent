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
#include <atomic>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <mutex>
#include <thread>
#include <memory>
#include <sstream>
#include <algorithm>
#include <tuple>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"

#include "logging.h"
#include "agent.h"
#include "sampling.h"
#include "utility.h"
#include "config.h"
#include "object_name.h"

namespace pinpoint {

    static std::string& global_agent_config_str() {
        static std::string cfg_str;
        return cfg_str;
    }

    static std::mutex& config_str_mutex() {
        static std::mutex mutex;
        return mutex;
    }

    static std::string& global_agent_config_file_path() {
        static std::string file_path;
        return file_path;
    }

    static std::mutex& config_file_path_mutex() {
        static std::mutex mutex;
        return mutex;
    }

    static std::string get_config_file_path_copy() {
        std::lock_guard<std::mutex> lock(config_file_path_mutex());
        return global_agent_config_file_path();
    }

    static std::thread& config_watcher_thread() {
        // Intentionally heap-allocated and never destroyed. This function-local
        // static is first used after the global agent registers its destructor,
        // so a plain `static std::thread` would be destroyed first at process
        // exit — while still joinable when the host never calls Shutdown() —
        // and a joinable std::thread destructor calls std::terminate().
        static auto* watcher = new std::thread();
        return *watcher;
    }

    static std::atomic<bool>& config_watcher_stop() {
        static std::atomic<bool> stop{false};
        return stop;
    }

    static std::mutex& config_watcher_mutex() {
        static std::mutex mutex;
        return mutex;
    }

    static void read_config_from_file(const char* config_file_path) {
        if (std::ifstream file(config_file_path); file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            {
                std::lock_guard<std::mutex> lock(config_str_mutex());
                global_agent_config_str() = buffer.str();
            }
            file.close();
        } else {
            LOG_ERROR("can't open config file = {}", config_file_path);
        }
    }

    void start_config_file_watcher() {
        std::lock_guard<std::mutex> lock(config_watcher_mutex());
        const auto path = get_config_file_path_copy();
        if (path.empty() || !std::filesystem::exists(path)) {
            return;
        }

        auto& watcher = config_watcher_thread();
        if (watcher.joinable()) {
            return;
        }
        config_watcher_stop().store(false);

        watcher = std::thread([path]() {
            // Seed with the non-throwing overload: the throwing form could
            // escape this thread function (the file may have been removed
            // between the exists() check above and the thread starting), and
            // an exception leaving a std::thread calls std::terminate(),
            // crashing the host. On error last_write_time stays default-
            // constructed, so the first iteration just treats the file as
            // changed and attempts a reload.
            std::error_code seed_ec;
            auto last_write_time = std::filesystem::last_write_time(path, seed_ec);

            while (!config_watcher_stop().load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));

                try {
                    auto current = std::filesystem::last_write_time(path);
                    if (current != last_write_time) {
                        last_write_time = current;
                        read_config_from_file(path.c_str());
                        auto new_cfg = make_config();
                        auto agent = GlobalAgent();
                        auto agent_impl = std::dynamic_pointer_cast<AgentImpl>(agent);

                        if (agent_impl && new_cfg && new_cfg->check()) {
                            const auto current_cfg = agent_impl->getConfig();
                            if (!current_cfg || new_cfg->isReloadable(current_cfg)) {
                                agent_impl->reloadConfig(new_cfg);
                                LOG_INFO("agent config reloaded");
                            } else {
                                LOG_ERROR("failed to reload agent config: config is not reloadable");
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_WARN("failed to watch config file: {}", e.what());
                } catch (...) {
                    LOG_WARN("failed to watch config file: unknown exception");
                }
            }
        });
    }

    void stop_config_file_watcher() {
        std::thread watcher_to_join;
        {
            std::lock_guard<std::mutex> lock(config_watcher_mutex());
            auto& watcher = config_watcher_thread();
            if (!watcher.joinable()) {
                return;
            }
            config_watcher_stop().store(true);
            watcher_to_join = std::move(watcher);
        }
        watcher_to_join.join();
    }

    // Each getter guards the whole lookup, not just the conversion: yaml-cpp
    // throws from the subscript itself (BadSubscript on scalar nodes) and from
    // element-level conversions (TypedBadConversion<Element> inside vector
    // decoding), all of which derive from YAML::Exception. A malformed config
    // must degrade to defaults, never throw into the embedding application.
    static bool get_boolean(const YAML::Node& yaml, std::string_view cname, bool default_value) {
        try {
            if (yaml[cname]) {
                return yaml[cname].as<bool>();
            }
        } catch (const YAML::Exception& e) {
            LOG_WARN("Failed to read '{}' as boolean: {}. Using default value: {}",
                     std::string(cname), e.what(), default_value);
        }

        return default_value;
    }

    static std::string get_string(const YAML::Node& yaml, std::string_view cname, std::string default_value) {
        try {
            if (yaml[cname]) {
                return yaml[cname].as<std::string>();
            }
        } catch (const YAML::Exception& e) {
            LOG_WARN("Failed to read '{}' as string: {}. Using default value: '{}'",
                     std::string(cname), e.what(), default_value);
        }

        return default_value;
    }

    static std::vector<std::string> get_string_vector(const YAML::Node& yaml, std::string_view cname,
                                                      std::vector<std::string> default_value) {
        try {
            if (yaml[cname]) {
                return yaml[cname].as<std::vector<std::string>>();
            }
        } catch (const YAML::Exception& e) {
            LOG_WARN("Failed to read '{}' as string vector: {}. Using default value",
                     std::string(cname), e.what());
        }

        return default_value;
    }

    static int get_int(const YAML::Node& yaml, std::string_view cname, int default_value) {
        try {
            if (yaml[cname]) {
                return yaml[cname].as<int>();
            }
        } catch (const YAML::Exception& e) {
            LOG_WARN("Failed to read '{}' as int: {}. Using default value: {}",
                     std::string(cname), e.what(), default_value);
        }

        return default_value;
    }

    static void load_grpc_channel_yaml(const YAML::Node& grpc, Config::GrpcChannelOptions& options) {
        options.ssl_enable = get_boolean(grpc, "SslEnable", options.ssl_enable);
        options.keepalive_time_ms = get_int(grpc, "KeepAliveTimeMs", options.keepalive_time_ms);
        options.keepalive_timeout_ms = get_int(grpc, "KeepAliveTimeoutMs", options.keepalive_timeout_ms);
        options.keepalive_permit_without_calls =
            get_boolean(grpc, "KeepAlivePermitWithoutCalls", options.keepalive_permit_without_calls);
        options.max_send_message_size = get_int(grpc, "MaxSendMessageSize", options.max_send_message_size);
        options.max_receive_message_size = get_int(grpc, "MaxReceiveMessageSize", options.max_receive_message_size);
        options.sender_queue_size = get_int(grpc, "SenderQueueSize", options.sender_queue_size);
        options.channel_executor_queue_size =
            get_int(grpc, "ChannelExecutorQueueSize", options.channel_executor_queue_size);
    }

    static void load_grpc_yaml(const YAML::Node& yaml, Config& config) {
        if (auto& grpc = yaml["Grpc"]) {
            if (auto& ssl = grpc["Ssl"]) {
                config.grpc.ssl.trust_cert_file_path =
                    get_string(ssl, "TrustCertFilePath", config.grpc.ssl.trust_cert_file_path);
                config.grpc.ssl.root_cert_file_path =
                    get_string(ssl, "RootCertFilePath", config.grpc.ssl.root_cert_file_path);
            }

            load_grpc_channel_yaml(grpc, config.grpc.channel);
        }
    }

    static double get_double(const YAML::Node& yaml, std::string_view cname, double default_value) {
        try {
            if (yaml[cname]) {
                return yaml[cname].as<double>();
            }
        } catch (const YAML::Exception& e) {
            LOG_WARN("Failed to read '{}' as double: {}. Using default value: {}",
                     std::string(cname), e.what(), default_value);
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
        config.uid_version_ = get_string(yaml, "UidVersion", "");
        config.service_name_ = get_string(yaml, "ServiceName", "");
        config.api_key_ = get_string(yaml, "ApiKey", "");

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

            if (auto& batch = span["Batch"]) {
                config.span.batch.size = get_int(batch, "Size", defaults::SPAN_BATCH_SIZE);
                config.span.batch.flush_interval_ms = get_int(batch, "FlushIntervalMs", defaults::SPAN_BATCH_FLUSH_INTERVAL_MS);
                config.span.batch.collect_deadline_ms = get_int(batch, "CollectDeadlineMs", defaults::SPAN_BATCH_COLLECT_DEADLINE_MS);
                config.span.batch.max_concurrent_requests = get_int(batch, "MaxConcurrentRequests", defaults::SPAN_BATCH_MAX_CONCURRENT_REQUESTS);
            }
        }

        if (auto& agent_info = yaml["AgentInfo"]) {
            config.agent_info.refresh_interval_ms = get_int(agent_info, "RefreshIntervalMs", defaults::AGENT_INFO_REFRESH_INTERVAL_MS);
            config.agent_info.send_retry_interval_ms = get_int(agent_info, "SendRetryIntervalMs", defaults::AGENT_INFO_SEND_RETRY_INTERVAL_MS);
            config.agent_info.max_try_per_attempt = get_int(agent_info, "MaxTryPerAttempt", defaults::AGENT_INFO_MAX_TRY_PER_ATTEMPT);
        }

        load_grpc_yaml(yaml, config);

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

    static void load_env_grpc_channel(Config::GrpcChannelOptions& options,
                                      const char* ssl_enable_env,
                                      const char* keepalive_time_env,
                                      const char* keepalive_timeout_env,
                                      const char* keepalive_permit_env,
                                      const char* max_send_env,
                                      const char* max_receive_env,
                                      const char* sender_queue_env,
                                      const char* channel_executor_queue_env) {
        if(const char* env_p = std::getenv(ssl_enable_env)) {
            options.ssl_enable = safe_env_stob(ssl_enable_env, env_p, options.ssl_enable);
        }
        if(const char* env_p = std::getenv(keepalive_time_env)) {
            options.keepalive_time_ms = safe_env_stoi(keepalive_time_env, env_p, options.keepalive_time_ms);
        }
        if(const char* env_p = std::getenv(keepalive_timeout_env)) {
            options.keepalive_timeout_ms = safe_env_stoi(keepalive_timeout_env, env_p, options.keepalive_timeout_ms);
        }
        if(const char* env_p = std::getenv(keepalive_permit_env)) {
            options.keepalive_permit_without_calls =
                safe_env_stob(keepalive_permit_env, env_p, options.keepalive_permit_without_calls);
        }
        if(const char* env_p = std::getenv(max_send_env)) {
            options.max_send_message_size = safe_env_stoi(max_send_env, env_p, options.max_send_message_size);
        }
        if(const char* env_p = std::getenv(max_receive_env)) {
            options.max_receive_message_size = safe_env_stoi(max_receive_env, env_p, options.max_receive_message_size);
        }
        if(const char* env_p = std::getenv(sender_queue_env)) {
            options.sender_queue_size = safe_env_stoi(sender_queue_env, env_p, options.sender_queue_size);
        }
        if(const char* env_p = std::getenv(channel_executor_queue_env)) {
            options.channel_executor_queue_size =
                safe_env_stoi(channel_executor_queue_env, env_p, options.channel_executor_queue_size);
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
        if(const char* env_p = std::getenv(env::UID_VERSION)) {
            config.uid_version_ = std::string(env_p);
        }
        if(const char* env_p = std::getenv(env::SERVICE_NAME)) {
            config.service_name_ = std::string(env_p);
        }
        if(const char* env_p = std::getenv(env::API_KEY)) {
            config.api_key_ = std::string(env_p);
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
        if(const char* env_p = std::getenv(env::SPAN_BATCH_SIZE)) {
            config.span.batch.size = safe_env_stoi(env::SPAN_BATCH_SIZE, env_p, defaults::SPAN_BATCH_SIZE);
        }
        if(const char* env_p = std::getenv(env::SPAN_BATCH_FLUSH_INTERVAL_MS)) {
            config.span.batch.flush_interval_ms = safe_env_stoi(env::SPAN_BATCH_FLUSH_INTERVAL_MS, env_p, defaults::SPAN_BATCH_FLUSH_INTERVAL_MS);
        }
        if(const char* env_p = std::getenv(env::SPAN_BATCH_COLLECT_DEADLINE_MS)) {
            config.span.batch.collect_deadline_ms = safe_env_stoi(env::SPAN_BATCH_COLLECT_DEADLINE_MS, env_p, defaults::SPAN_BATCH_COLLECT_DEADLINE_MS);
        }
        if(const char* env_p = std::getenv(env::SPAN_BATCH_MAX_CONCURRENT_REQUESTS)) {
            config.span.batch.max_concurrent_requests = safe_env_stoi(env::SPAN_BATCH_MAX_CONCURRENT_REQUESTS, env_p, defaults::SPAN_BATCH_MAX_CONCURRENT_REQUESTS);
        }
        if(const char* env_p = std::getenv(env::AGENT_INFO_REFRESH_INTERVAL_MS)) {
            config.agent_info.refresh_interval_ms = safe_env_stoi(env::AGENT_INFO_REFRESH_INTERVAL_MS, env_p, defaults::AGENT_INFO_REFRESH_INTERVAL_MS);
        }
        if(const char* env_p = std::getenv(env::AGENT_INFO_SEND_RETRY_INTERVAL_MS)) {
            config.agent_info.send_retry_interval_ms = safe_env_stoi(env::AGENT_INFO_SEND_RETRY_INTERVAL_MS, env_p, defaults::AGENT_INFO_SEND_RETRY_INTERVAL_MS);
        }
        if(const char* env_p = std::getenv(env::AGENT_INFO_MAX_TRY_PER_ATTEMPT)) {
            config.agent_info.max_try_per_attempt = safe_env_stoi(env::AGENT_INFO_MAX_TRY_PER_ATTEMPT, env_p, defaults::AGENT_INFO_MAX_TRY_PER_ATTEMPT);
        }

        if(const char* env_p = std::getenv(env::GRPC_SSL_TRUST_CERT_FILE_PATH)) {
            config.grpc.ssl.trust_cert_file_path = std::string(env_p);
        }
        if(const char* env_p = std::getenv(env::GRPC_SSL_ROOT_CERT_FILE_PATH)) {
            config.grpc.ssl.root_cert_file_path = std::string(env_p);
        }
        load_env_grpc_channel(config.grpc.channel,
                              env::GRPC_SSL_ENABLE,
                              env::GRPC_KEEPALIVE_TIME_MS,
                              env::GRPC_KEEPALIVE_TIMEOUT_MS,
                              env::GRPC_KEEPALIVE_PERMIT_WITHOUT_CALLS,
                              env::GRPC_MAX_SEND_MESSAGE_SIZE,
                              env::GRPC_MAX_RECEIVE_MESSAGE_SIZE,
                              env::GRPC_SENDER_QUEUE_SIZE,
                              env::GRPC_CHANNEL_EXECUTOR_QUEUE_SIZE);

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

    void set_config_string(std::string_view cfg_str) {
        std::lock_guard<std::mutex> lock(config_str_mutex());
        global_agent_config_str() = cfg_str;
    }

    void set_config_file_path(std::string_view file_path) {
        {
            std::lock_guard<std::mutex> lock(config_file_path_mutex());
            global_agent_config_file_path() = file_path;
        }
    }

    constexpr int MIN_PORT = 1;
    constexpr int MAX_PORT = 65535;
    constexpr int NONE_SAMPLING_COUNTER_RATE = 0;
    constexpr double NONE_SAMPLING_PERCENT_RATE = 0.0;
    constexpr int NONE_SAMPLING_NEW_THROUGHPUT = 0;
    constexpr int NONE_SAMPLING_CONTINUE_THROUGHPUT = 0;
    constexpr double MIN_SAMPLING_PERCENT_RATE = 0.01;
    constexpr double MAX_SAMPLING_PERCENT_RATE = 100.0;
    constexpr int MIN_SPAN_QUEUE_SIZE = 1;
    constexpr int MAX_SPAN_QUEUE_SIZE = 65536;
    constexpr int UNLIMITED_SIZE = -1;
    constexpr int MIN_SPAN_EVENT_DEPTH = 2;
    constexpr int MIN_SPAN_EVENT_SEQUENCE = 4;
    constexpr int MIN_SPAN_EVENT_CHUNK_SIZE = 1;
    constexpr int MAX_SPAN_EVENT_DEPTH = INT32_MAX;
    constexpr int MAX_SPAN_EVENT_SEQUENCE = INT32_MAX;
    constexpr int MIN_STAT_BATCH_COUNT = 1;
    constexpr int MAX_STAT_BATCH_COUNT = 100;
    constexpr int MIN_STAT_INTERVAL_MS = 1000;
    constexpr int MAX_STAT_INTERVAL_MS = 60000;
    constexpr int MIN_GRPC_QUEUE_SIZE = 1;
    constexpr int MAX_GRPC_QUEUE_SIZE = 65536;

    static int clamp_port(int port, int default_port) {
        if (port < MIN_PORT || port > MAX_PORT) {
            LOG_WARN("port {} is out of range ({}-{}), using default: {}", port, MIN_PORT, MAX_PORT, default_port);
            return default_port;
        }
        return port;
    }

    static void validate_grpc_channel(Config::GrpcChannelOptions& options, const char* name,
                                      const Config::GrpcChannelOptions& defaults) {
        if (options.keepalive_time_ms < 0) {
            LOG_WARN("{} grpc keepalive time {}ms is invalid, using default: {}ms",
                     name, options.keepalive_time_ms, defaults.keepalive_time_ms);
            options.keepalive_time_ms = defaults.keepalive_time_ms;
        }
        if (options.keepalive_timeout_ms < 0) {
            LOG_WARN("{} grpc keepalive timeout {}ms is invalid, using default: {}ms",
                     name, options.keepalive_timeout_ms, defaults.keepalive_timeout_ms);
            options.keepalive_timeout_ms = defaults.keepalive_timeout_ms;
        }
        if (options.max_send_message_size < UNLIMITED_SIZE) {
            LOG_WARN("{} grpc max send message size {} is invalid, using default: {}",
                     name, options.max_send_message_size, defaults.max_send_message_size);
            options.max_send_message_size = defaults.max_send_message_size;
        }
        if (options.max_receive_message_size < UNLIMITED_SIZE) {
            LOG_WARN("{} grpc max receive message size {} is invalid, using default: {}",
                     name, options.max_receive_message_size, defaults.max_receive_message_size);
            options.max_receive_message_size = defaults.max_receive_message_size;
        }
        if (options.sender_queue_size < MIN_GRPC_QUEUE_SIZE || options.sender_queue_size > MAX_GRPC_QUEUE_SIZE) {
            LOG_WARN("{} grpc sender queue size {} is out of range ({}-{}), using default: {}",
                     name, options.sender_queue_size, MIN_GRPC_QUEUE_SIZE, MAX_GRPC_QUEUE_SIZE, defaults.sender_queue_size);
            options.sender_queue_size = defaults.sender_queue_size;
        }
        if (options.channel_executor_queue_size < MIN_GRPC_QUEUE_SIZE ||
            options.channel_executor_queue_size > MAX_GRPC_QUEUE_SIZE) {
            LOG_WARN("{} grpc channel executor queue size {} is out of range ({}-{}), using default: {}",
                     name, options.channel_executor_queue_size, MIN_GRPC_QUEUE_SIZE, MAX_GRPC_QUEUE_SIZE,
                     defaults.channel_executor_queue_size);
            options.channel_executor_queue_size = defaults.channel_executor_queue_size;
        }
    }

    // make_config() is reached from the public CreateAgent() entry points, so
    // it must never let a parsing problem escape into the host application:
    // yaml errors degrade to defaults, and the function-level handler below is
    // the last-resort backstop.
    std::shared_ptr<Config> make_config() try {
        auto config = std::make_shared<Config>();
        bool is_container_set = false;

        if(const char* env_p = std::getenv(env::CONFIG_FILE); env_p != nullptr) {
            set_config_file_path(env_p);
        }
        const auto config_path = get_config_file_path_copy();
        if(!config_path.empty()) {
            read_config_from_file(config_path.c_str());
        }

        YAML::Node yaml;
        std::string user_config;
        {
            std::lock_guard<std::mutex> lock(config_str_mutex());
            user_config = global_agent_config_str();
        }
        if (!user_config.empty()) {
            try {
                yaml = YAML::Load(user_config);
            } catch (const YAML::Exception& e) {
                LOG_ERROR("yaml parsing exception = {} - continuing with defaults", e.what());
            }
        }

        try {
            load_yaml_config(yaml, *config, is_container_set);
        } catch (const std::exception& e) {
            // E.g. a section node of the wrong shape (BadSubscript) that the
            // per-key getters cannot intercept. Keep whatever was parsed so
            // far and continue with defaults plus environment overrides.
            LOG_ERROR("failed to load yaml config: {} - continuing with defaults", e.what());
        }
        // Environment variables are process-level identity/bootstrap inputs and
        // are only meant to seed the very first configuration. Once an agent is
        // already running, make_config() is being called to rebuild the config
        // from an updated file (a reload), so env overrides must not be
        // re-applied — otherwise they would silently override values the user
        // just changed in the config file.
        if (!global_agent_exists()) {
            load_env_config(*config, is_container_set);
        }

        if (!config->log.file_path.empty()) {
            Logger::getInstance().setFileLogger(config->log.file_path, config->log.max_file_size);
        }
        Logger::getInstance().setLogLevel(config->log.level);

        // Resolve agent self-identity (ObjectName) according to the configured
        // uid version. Mirrors Java ObjectNameResolver{V1,V4}. The resolver owns
        // version-aware validation (e.g. applicationName <=24 for v1 vs <=254 for
        // v3, which check() cannot distinguish since both map to version 1).
        {
            const auto name_version = parse_name_version(config->uid_version_);
            config->object_name_version_ =
                (name_version == NameVersion::kV4) ? object_name::VERSION_V4
                                                   : object_name::VERSION_V1;

            ObjectNameInput in;
            in.agent_id = config->agent_id_;
            in.agent_name = config->agent_name_;
            in.application_name = config->app_name_;
            in.service_name = config->service_name_;
            in.api_key = config->api_key_;

            if (auto object_name = resolve_object_name(name_version, in)) {
                config->agent_id_ = object_name->agent_id;
                config->agent_name_ = object_name->agent_name;
                config->app_name_ = object_name->application_name;
                config->service_name_ = object_name->service_name;
                config->api_key_ = object_name->api_key;
                config->identity_resolved_ = true;
            } else {
                // A required identity value is missing/invalid. Keep returning a
                // populated config (callers may inspect it); Config::check() fails
                // and CreateAgent() degrades to a noop agent. Ensure agent_id is
                // populated for diagnostic logging.
                LOG_ERROR("failed to resolve agent identity (uid.version='{}')",
                          config->uid_version_.empty() ? "v3" : config->uid_version_);
                if (config->agent_id_.empty()) {
                    config->agent_id_ = base64_encode_uuid(generate_uuid_v7());
                }
                config->identity_resolved_ = false;
            }
        }

        config->collector.agent_port = clamp_port(config->collector.agent_port, defaults::AGENT_PORT);
        config->collector.span_port = clamp_port(config->collector.span_port, defaults::SPAN_PORT);
        config->collector.stat_port = clamp_port(config->collector.stat_port, defaults::STAT_PORT);

        if (config->stat.batch_count < MIN_STAT_BATCH_COUNT || config->stat.batch_count > MAX_STAT_BATCH_COUNT) {
            LOG_WARN("stat batch count {} is out of range ({}-{}), using default: {}",
                     config->stat.batch_count, MIN_STAT_BATCH_COUNT, MAX_STAT_BATCH_COUNT, defaults::STAT_BATCH_COUNT);
            config->stat.batch_count = defaults::STAT_BATCH_COUNT;
        }
        if (config->stat.collect_interval < MIN_STAT_INTERVAL_MS || config->stat.collect_interval > MAX_STAT_INTERVAL_MS) {
            LOG_WARN("stat collect interval {}ms is out of range ({}-{}ms), using default: {}ms",
                     config->stat.collect_interval, MIN_STAT_INTERVAL_MS, MAX_STAT_INTERVAL_MS, defaults::STAT_INTERVAL_MS);
            config->stat.collect_interval = defaults::STAT_INTERVAL_MS;
        }

        if (config->sampling.counter_rate < NONE_SAMPLING_COUNTER_RATE) {
            LOG_WARN("sampling counter rate {} is invalid, using default: {}",
                     config->sampling.counter_rate, NONE_SAMPLING_COUNTER_RATE);
            config->sampling.counter_rate = NONE_SAMPLING_COUNTER_RATE;
        }
        if (config->sampling.percent_rate < NONE_SAMPLING_PERCENT_RATE) {
            LOG_WARN("sampling percent rate {} is invalid, using default: {}",
                     config->sampling.percent_rate, NONE_SAMPLING_PERCENT_RATE);
            config->sampling.percent_rate = NONE_SAMPLING_PERCENT_RATE;
        } else if (config->sampling.percent_rate < MIN_SAMPLING_PERCENT_RATE) {
            LOG_WARN("sampling percent rate {} is below minimum, clamping to: {}",
                     config->sampling.percent_rate, MIN_SAMPLING_PERCENT_RATE);
            config->sampling.percent_rate = MIN_SAMPLING_PERCENT_RATE;
        } else if (config->sampling.percent_rate > MAX_SAMPLING_PERCENT_RATE) {
            LOG_WARN("sampling percent rate {} exceeds maximum, clamping to: {}",
                     config->sampling.percent_rate, MAX_SAMPLING_PERCENT_RATE);
            config->sampling.percent_rate = MAX_SAMPLING_PERCENT_RATE;
        }
        if (config->sampling.new_throughput < NONE_SAMPLING_NEW_THROUGHPUT) {
            LOG_WARN("sampling new throughput {} is invalid, using default: {}",
                     config->sampling.new_throughput, NONE_SAMPLING_NEW_THROUGHPUT);
            config->sampling.new_throughput = NONE_SAMPLING_NEW_THROUGHPUT;
        }
        if (config->sampling.cont_throughput < NONE_SAMPLING_CONTINUE_THROUGHPUT) {
            LOG_WARN("sampling continue throughput {} is invalid, using default: {}",
                     config->sampling.cont_throughput, NONE_SAMPLING_CONTINUE_THROUGHPUT);
            config->sampling.cont_throughput = NONE_SAMPLING_CONTINUE_THROUGHPUT;
        }

        if (config->span.queue_size < MIN_SPAN_QUEUE_SIZE || config->span.queue_size > MAX_SPAN_QUEUE_SIZE) {
            LOG_WARN("span queue size {} is out of range ({}-{}), using default: {}",
                     config->span.queue_size, MIN_SPAN_QUEUE_SIZE, MAX_SPAN_QUEUE_SIZE, defaults::SPAN_QUEUE_SIZE);
            config->span.queue_size = defaults::SPAN_QUEUE_SIZE;
        }
        if (config->span.max_event_depth == UNLIMITED_SIZE) {
            config->span.max_event_depth = MAX_SPAN_EVENT_DEPTH;
        } else if (config->span.max_event_depth < MIN_SPAN_EVENT_DEPTH) {
            LOG_WARN("span max event depth {} is below minimum, clamping to: {}",
                     config->span.max_event_depth, MIN_SPAN_EVENT_DEPTH);
            config->span.max_event_depth = MIN_SPAN_EVENT_DEPTH;
        }
        if (config->span.max_event_sequence == UNLIMITED_SIZE) {
            config->span.max_event_sequence = MAX_SPAN_EVENT_SEQUENCE;
        } else if (config->span.max_event_sequence < MIN_SPAN_EVENT_SEQUENCE) {
            LOG_WARN("span max event sequence {} is below minimum, clamping to: {}",
                     config->span.max_event_sequence, MIN_SPAN_EVENT_SEQUENCE);
            config->span.max_event_sequence = MIN_SPAN_EVENT_SEQUENCE;
        }
        if (config->span.event_chunk_size < MIN_SPAN_EVENT_CHUNK_SIZE) {
            LOG_WARN("span event chunk size {} is below minimum, clamping to: {}",
                     config->span.event_chunk_size, defaults::SPAN_EVENT_CHUNK_SIZE);
            config->span.event_chunk_size = defaults::SPAN_EVENT_CHUNK_SIZE;
        }

        if (config->span.batch.size < 1) {
            LOG_WARN("span batch size {} is invalid, using default: {}",
                     config->span.batch.size, defaults::SPAN_BATCH_SIZE);
            config->span.batch.size = defaults::SPAN_BATCH_SIZE;
        }
        if (config->span.batch.flush_interval_ms < 1) {
            LOG_WARN("span batch flush interval {}ms is invalid, using default: {}ms",
                     config->span.batch.flush_interval_ms, defaults::SPAN_BATCH_FLUSH_INTERVAL_MS);
            config->span.batch.flush_interval_ms = defaults::SPAN_BATCH_FLUSH_INTERVAL_MS;
        }
        if (config->span.batch.collect_deadline_ms < 0) {
            LOG_WARN("span batch collect deadline {}ms is invalid, using default: {}ms",
                     config->span.batch.collect_deadline_ms, defaults::SPAN_BATCH_COLLECT_DEADLINE_MS);
            config->span.batch.collect_deadline_ms = defaults::SPAN_BATCH_COLLECT_DEADLINE_MS;
        }
        if (config->span.batch.max_concurrent_requests < 1) {
            LOG_WARN("span batch max concurrent requests {} is invalid, using default: {}",
                     config->span.batch.max_concurrent_requests, defaults::SPAN_BATCH_MAX_CONCURRENT_REQUESTS);
            config->span.batch.max_concurrent_requests = defaults::SPAN_BATCH_MAX_CONCURRENT_REQUESTS;
        }
        if (config->agent_info.refresh_interval_ms < 1) {
            LOG_WARN("agent info refresh interval {}ms is invalid, using default: {}ms",
                     config->agent_info.refresh_interval_ms, defaults::AGENT_INFO_REFRESH_INTERVAL_MS);
            config->agent_info.refresh_interval_ms = defaults::AGENT_INFO_REFRESH_INTERVAL_MS;
        }
        if (config->agent_info.send_retry_interval_ms < 1) {
            LOG_WARN("agent info send retry interval {}ms is invalid, using default: {}ms",
                     config->agent_info.send_retry_interval_ms, defaults::AGENT_INFO_SEND_RETRY_INTERVAL_MS);
            config->agent_info.send_retry_interval_ms = defaults::AGENT_INFO_SEND_RETRY_INTERVAL_MS;
        }
        if (config->agent_info.max_try_per_attempt < 1) {
            LOG_WARN("agent info max try per attempt {} is invalid, using default: {}",
                     config->agent_info.max_try_per_attempt, defaults::AGENT_INFO_MAX_TRY_PER_ATTEMPT);
            config->agent_info.max_try_per_attempt = defaults::AGENT_INFO_MAX_TRY_PER_ATTEMPT;
        }

        // A negative limit would cast to a huge size_t at the use site
        // (UrlStatSnapshot::add), disabling the cap and letting the URL map grow
        // unbounded with cardinality. Reject it.
        if (config->http.url_stat.limit < 0) {
            LOG_WARN("http url stat limit {} is invalid, using default: {}",
                     config->http.url_stat.limit, defaults::HTTP_URL_STAT_LIMIT);
            config->http.url_stat.limit = defaults::HTTP_URL_STAT_LIMIT;
        }

        validate_grpc_channel(config->grpc.channel, "grpc", Config::GrpcChannelOptions());

        if (!is_container_set) {
            config->is_container = is_container_env();
        }

        LOG_INFO("config: {}", "\n" + to_config_string(*config));
        return config;
    } catch (const std::exception& e) {
        try { LOG_ERROR("make config exception = {}", e.what()); } catch (...) {}
        return nullptr;
    } catch (...) {
        try { LOG_ERROR("make config unknown exception"); } catch (...) {}
        return nullptr;
    }

    namespace {
        template <typename T>
        std::string config_value_to_string(const T& value) {
            YAML::Emitter emitter;
            emitter << value;
            return emitter.c_str();
        }

        std::string config_value_to_string(const std::vector<std::string>& values) {
            YAML::Emitter emitter;
            emitter << YAML::Flow << YAML::BeginSeq;
            for (const auto& value : values) {
                emitter << value;
            }
            emitter << YAML::EndSeq;
            return emitter.c_str();
        }

        template <typename T>
        void add_non_default_config(std::vector<std::string>& config_strings,
                                    const char* key,
                                    const T& value,
                                    const T& default_value) {
            if (value != default_value) {
                config_strings.push_back(absl::StrCat(key, "=", config_value_to_string(value)));
            }
        }
    }

    std::vector<std::string> to_non_default_config_strings(const Config& config) {
        const Config default_config;
        std::vector<std::string> config_strings;
        config_strings.reserve(64);

        add_non_default_config(config_strings, "UidVersion", config.uid_version_, default_config.uid_version_);
        add_non_default_config(config_strings, "Log.Level", config.log.level, default_config.log.level);
        add_non_default_config(config_strings, "Log.FilePath", config.log.file_path, default_config.log.file_path);
        add_non_default_config(config_strings, "Log.MaxFileSize", config.log.max_file_size, default_config.log.max_file_size);
        add_non_default_config(config_strings, "Grpc.Ssl.TrustCertFilePath", config.grpc.ssl.trust_cert_file_path,
                               default_config.grpc.ssl.trust_cert_file_path);
        add_non_default_config(config_strings, "Grpc.Ssl.RootCertFilePath", config.grpc.ssl.root_cert_file_path,
                               default_config.grpc.ssl.root_cert_file_path);
        add_non_default_config(config_strings, "Grpc.SslEnable", config.grpc.channel.ssl_enable,
                               default_config.grpc.channel.ssl_enable);
        add_non_default_config(config_strings, "Grpc.KeepAliveTimeMs", config.grpc.channel.keepalive_time_ms,
                               default_config.grpc.channel.keepalive_time_ms);
        add_non_default_config(config_strings, "Grpc.KeepAliveTimeoutMs", config.grpc.channel.keepalive_timeout_ms,
                               default_config.grpc.channel.keepalive_timeout_ms);
        add_non_default_config(config_strings, "Grpc.KeepAlivePermitWithoutCalls",
                               config.grpc.channel.keepalive_permit_without_calls,
                               default_config.grpc.channel.keepalive_permit_without_calls);
        add_non_default_config(config_strings, "Grpc.MaxSendMessageSize", config.grpc.channel.max_send_message_size,
                               default_config.grpc.channel.max_send_message_size);
        add_non_default_config(config_strings, "Grpc.MaxReceiveMessageSize", config.grpc.channel.max_receive_message_size,
                               default_config.grpc.channel.max_receive_message_size);
        add_non_default_config(config_strings, "Grpc.SenderQueueSize", config.grpc.channel.sender_queue_size,
                               default_config.grpc.channel.sender_queue_size);
        add_non_default_config(config_strings, "Grpc.ChannelExecutorQueueSize",
                               config.grpc.channel.channel_executor_queue_size,
                               default_config.grpc.channel.channel_executor_queue_size);
        add_non_default_config(config_strings, "Stat.Enable", config.stat.enable, default_config.stat.enable);
        add_non_default_config(config_strings, "Stat.BatchCount", config.stat.batch_count, default_config.stat.batch_count);
        add_non_default_config(config_strings, "Stat.BatchInterval", config.stat.collect_interval,
                               default_config.stat.collect_interval);
        add_non_default_config(config_strings, "Sampling.Type", config.sampling.type, default_config.sampling.type);
        add_non_default_config(config_strings, "Sampling.CounterRate", config.sampling.counter_rate,
                               default_config.sampling.counter_rate);
        add_non_default_config(config_strings, "Sampling.PercentRate", config.sampling.percent_rate,
                               default_config.sampling.percent_rate);
        add_non_default_config(config_strings, "Sampling.NewThroughput", config.sampling.new_throughput,
                               default_config.sampling.new_throughput);
        add_non_default_config(config_strings, "Sampling.ContinueThroughput", config.sampling.cont_throughput,
                               default_config.sampling.cont_throughput);
        add_non_default_config(config_strings, "Span.QueueSize", config.span.queue_size, default_config.span.queue_size);
        add_non_default_config(config_strings, "Span.MaxEventDepth", config.span.max_event_depth,
                               default_config.span.max_event_depth);
        add_non_default_config(config_strings, "Span.MaxEventSequence", config.span.max_event_sequence,
                               default_config.span.max_event_sequence);
        add_non_default_config(config_strings, "Span.EventChunkSize", config.span.event_chunk_size,
                               default_config.span.event_chunk_size);
        add_non_default_config(config_strings, "Span.Batch.Size", config.span.batch.size,
                               default_config.span.batch.size);
        add_non_default_config(config_strings, "Span.Batch.FlushIntervalMs", config.span.batch.flush_interval_ms,
                               default_config.span.batch.flush_interval_ms);
        add_non_default_config(config_strings, "Span.Batch.CollectDeadlineMs", config.span.batch.collect_deadline_ms,
                               default_config.span.batch.collect_deadline_ms);
        add_non_default_config(config_strings, "Span.Batch.MaxConcurrentRequests",
                               config.span.batch.max_concurrent_requests,
                               default_config.span.batch.max_concurrent_requests);
        add_non_default_config(config_strings, "AgentInfo.RefreshIntervalMs", config.agent_info.refresh_interval_ms,
                               default_config.agent_info.refresh_interval_ms);
        add_non_default_config(config_strings, "AgentInfo.SendRetryIntervalMs", config.agent_info.send_retry_interval_ms,
                               default_config.agent_info.send_retry_interval_ms);
        add_non_default_config(config_strings, "AgentInfo.MaxTryPerAttempt", config.agent_info.max_try_per_attempt,
                               default_config.agent_info.max_try_per_attempt);
        add_non_default_config(config_strings, "Http.CollectUrlStat", config.http.url_stat.enable,
                               default_config.http.url_stat.enable);
        add_non_default_config(config_strings, "Http.UrlStatLimit", config.http.url_stat.limit,
                               default_config.http.url_stat.limit);
        add_non_default_config(config_strings, "Http.UrlStatEnableTrimPath", config.http.url_stat.enable_trim_path,
                               default_config.http.url_stat.enable_trim_path);
        add_non_default_config(config_strings, "Http.UrlStatTrimPathDepth", config.http.url_stat.trim_path_depth,
                               default_config.http.url_stat.trim_path_depth);
        add_non_default_config(config_strings, "Http.UrlStatMethodPrefix", config.http.url_stat.method_prefix,
                               default_config.http.url_stat.method_prefix);
        add_non_default_config(config_strings, "Http.Server.StatusCodeErrors", config.http.server.status_errors,
                               default_config.http.server.status_errors);
        add_non_default_config(config_strings, "Http.Server.ExcludeUrl", config.http.server.exclude_url,
                               default_config.http.server.exclude_url);
        add_non_default_config(config_strings, "Http.Server.ExcludeMethod", config.http.server.exclude_method,
                               default_config.http.server.exclude_method);
        add_non_default_config(config_strings, "Http.Server.RecordRequestHeader", config.http.server.rec_request_header,
                               default_config.http.server.rec_request_header);
        add_non_default_config(config_strings, "Http.Server.RecordRequestCookie", config.http.server.rec_request_cookie,
                               default_config.http.server.rec_request_cookie);
        add_non_default_config(config_strings, "Http.Server.RecordResponseHeader", config.http.server.rec_response_header,
                               default_config.http.server.rec_response_header);
        add_non_default_config(config_strings, "Http.Client.RecordRequestHeader", config.http.client.rec_request_header,
                               default_config.http.client.rec_request_header);
        add_non_default_config(config_strings, "Http.Client.RecordRequestCookie", config.http.client.rec_request_cookie,
                               default_config.http.client.rec_request_cookie);
        add_non_default_config(config_strings, "Http.Client.RecordResponseHeader", config.http.client.rec_response_header,
                               default_config.http.client.rec_response_header);
        add_non_default_config(config_strings, "Sql.MaxBindArgsSize", config.sql.max_bind_args_size,
                               default_config.sql.max_bind_args_size);
        add_non_default_config(config_strings, "Sql.EnableSqlStats", config.sql.enable_sql_stats,
                               default_config.sql.enable_sql_stats);
        add_non_default_config(config_strings, "EnableCallstackTrace", config.enable_callstack_trace,
                               default_config.enable_callstack_trace);

        return config_strings;
    }

    std::string to_config_string(const Config& config) {
        YAML::Emitter emitter;

        emitter << YAML::BeginMap;
        emitter << YAML::Key << "ApplicationName" << YAML::Value << config.app_name_;
        emitter << YAML::Key << "ApplicationType" << YAML::Value << config.app_type_;
        emitter << YAML::Key << "AgentId" << YAML::Value << config.agent_id_;
        emitter << YAML::Key << "AgentName" << YAML::Value << config.agent_name_;
        emitter << YAML::Key << "UidVersion" << YAML::Value << config.uid_version_;
        if (config.is_v4()) {
            emitter << YAML::Key << "ServiceName" << YAML::Value << config.service_name_;
            // ApiKey is intentionally masked and never serialized in plaintext.
            emitter << YAML::Key << "ApiKey" << YAML::Value << (config.api_key_.empty() ? "" : "****");
        }

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

        auto emit_grpc_channel = [&emitter](const Config::GrpcChannelOptions& options) {
            emitter << YAML::Key << "SslEnable" << YAML::Value << options.ssl_enable;
            emitter << YAML::Key << "KeepAliveTimeMs" << YAML::Value << options.keepalive_time_ms;
            emitter << YAML::Key << "KeepAliveTimeoutMs" << YAML::Value << options.keepalive_timeout_ms;
            emitter << YAML::Key << "KeepAlivePermitWithoutCalls" << YAML::Value << options.keepalive_permit_without_calls;
            emitter << YAML::Key << "MaxSendMessageSize" << YAML::Value << options.max_send_message_size;
            emitter << YAML::Key << "MaxReceiveMessageSize" << YAML::Value << options.max_receive_message_size;
            emitter << YAML::Key << "SenderQueueSize" << YAML::Value << options.sender_queue_size;
            emitter << YAML::Key << "ChannelExecutorQueueSize" << YAML::Value << options.channel_executor_queue_size;
        };

        emitter << YAML::Key << "Grpc";
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "Ssl";
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "TrustCertFilePath" << YAML::Value << config.grpc.ssl.trust_cert_file_path;
        emitter << YAML::Key << "RootCertFilePath" << YAML::Value << config.grpc.ssl.root_cert_file_path;
        emitter << YAML::EndMap;
        emit_grpc_channel(config.grpc.channel);
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
        emitter << YAML::Key << "ContinueThroughput" << YAML::Value << config.sampling.cont_throughput;
        emitter << YAML::EndMap;

        emitter << YAML::Key << "Span";
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "QueueSize" << YAML::Value << config.span.queue_size;
        emitter << YAML::Key << "MaxEventDepth" << YAML::Value << config.span.max_event_depth;
        emitter << YAML::Key << "MaxEventSequence" << YAML::Value << config.span.max_event_sequence;
        emitter << YAML::Key << "EventChunkSize" << YAML::Value << config.span.event_chunk_size;
        emitter << YAML::Key << "Batch";
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "Size" << YAML::Value << config.span.batch.size;
        emitter << YAML::Key << "FlushIntervalMs" << YAML::Value << config.span.batch.flush_interval_ms;
        emitter << YAML::Key << "CollectDeadlineMs" << YAML::Value << config.span.batch.collect_deadline_ms;
        emitter << YAML::Key << "MaxConcurrentRequests" << YAML::Value << config.span.batch.max_concurrent_requests;
        emitter << YAML::EndMap;
        emitter << YAML::EndMap;

        emitter << YAML::Key << "AgentInfo";
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "RefreshIntervalMs" << YAML::Value << config.agent_info.refresh_interval_ms;
        emitter << YAML::Key << "SendRetryIntervalMs" << YAML::Value << config.agent_info.send_retry_interval_ms;
        emitter << YAML::Key << "MaxTryPerAttempt" << YAML::Value << config.agent_info.max_try_per_attempt;
        emitter << YAML::EndMap;

        emitter << YAML::Key << "Http";
        emitter << YAML::BeginMap;

        emitter << YAML::Key << "CollectUrlStat" << YAML::Value << config.http.url_stat.enable;
        emitter << YAML::Key << "UrlStatLimit" << YAML::Value << config.http.url_stat.limit;
        emitter << YAML::Key << "UrlStatEnableTrimPath" << YAML::Value << config.http.url_stat.enable_trim_path;
        emitter << YAML::Key << "UrlStatTrimPathDepth" << YAML::Value << config.http.url_stat.trim_path_depth;
        emitter << YAML::Key << "UrlStatMethodPrefix" << YAML::Value << config.http.url_stat.method_prefix;

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
        // Identity fields (agent_id_/agent_name_/app_name_ and, for v4,
        // service_name_/api_key_) are validated and length-checked per uid version
        // by make_config()'s resolve_object_name() — version-aware (e.g. the v1 vs
        // v3 applicationName limit) in a way check() cannot reproduce here. A failed
        // resolution sets identity_resolved_ = false and aborts startup.
        if (!identity_resolved_) {
            LOG_ERROR("agent identity resolution failed");
            return false;
        }

        return true;
    }

    static bool same_grpc_channel(const Config::GrpcChannelOptions& lhs,
                                  const Config::GrpcChannelOptions& rhs) {
        return std::tie(lhs.ssl_enable,
                        lhs.keepalive_time_ms,
                        lhs.keepalive_timeout_ms,
                        lhs.keepalive_permit_without_calls,
                        lhs.max_send_message_size,
                        lhs.max_receive_message_size,
                        lhs.sender_queue_size,
                        lhs.channel_executor_queue_size) ==
               std::tie(rhs.ssl_enable,
                        rhs.keepalive_time_ms,
                        rhs.keepalive_timeout_ms,
                        rhs.keepalive_permit_without_calls,
                        rhs.max_send_message_size,
                        rhs.max_receive_message_size,
                        rhs.sender_queue_size,
                        rhs.channel_executor_queue_size);
    }

    static bool same_grpc_config(const Config& lhs, const Config& rhs) {
        return std::tie(lhs.grpc.ssl.trust_cert_file_path,
                        lhs.grpc.ssl.root_cert_file_path) ==
               std::tie(rhs.grpc.ssl.trust_cert_file_path,
                        rhs.grpc.ssl.root_cert_file_path) &&
               same_grpc_channel(lhs.grpc.channel, rhs.grpc.channel);
    }

    bool Config::isReloadable(const std::shared_ptr<const Config>& old) const {
        if (!old) return true;
        return std::tie(app_name_, app_type_, agent_id_, agent_name_,
                        uid_version_, service_name_, api_key_, object_name_version_,
                        collector.host, collector.agent_port, collector.span_port, collector.stat_port) ==
               std::tie(old->app_name_, old->app_type_, old->agent_id_, old->agent_name_,
                        old->uid_version_, old->service_name_, old->api_key_, old->object_name_version_,
                        old->collector.host, old->collector.agent_port, old->collector.span_port, old->collector.stat_port) &&
               same_grpc_config(*this, *old);
    }
}
