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
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <system_error>
#include <mutex>
#include <thread>
#include <memory>
#include <sstream>
#include <algorithm>
#include <tuple>
#include <unistd.h>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"

#include "logging.h"
#include "sampling.h"
#include "utility.h"
#include "config.h"
#include "object_name.h"

namespace pinpoint {

    // Returns the effective env var prefix for the options: the configured
    // one, or the default when unset.
    static std::string effective_env_prefix(const AgentOptions& options) {
        return options.env_prefix.empty() ? std::string(env::DEFAULT_PREFIX)
                                          : options.env_prefix;
    }

    // Builds the full env var name from a suffix (e.g. "APPLICATION_NAME") by
    // prepending the active prefix: "<prefix>_<suffix>".
    static std::string resolve_env_name(const std::string& prefix, const char* suffix) {
        return prefix + "_" + suffix;
    }

    // Result of looking up a prefix-resolved env var: the fully resolved name
    // (for accurate logging) and the value (null when unset).
    struct ResolvedEnv {
        std::string name;
        const char* value;
        explicit operator bool() const { return value != nullptr; }
    };

    static ResolvedEnv get_env(const std::string& prefix, const char* suffix) {
        std::string name = resolve_env_name(prefix, suffix);
        const char* value = std::getenv(name.c_str());
        return {std::move(name), value};
    }

    // Stop signal for one watcher generation. Each started watcher captures
    // its own signal by shared_ptr, so a stop issued to one generation can
    // never be undone by a later start: with a single shared signal, a
    // start() racing stop() (which joins outside the lock) could reset it
    // before the old watcher — possibly still waiting out its poll tick —
    // ever observed it, leaving that watcher running forever and the stopper
    // blocked in join().
    //
    // A condition variable rather than a plain sleep+atomic, so a stop request
    // wakes the watcher immediately: with an uninterruptible poll-tick sleep,
    // stop() — and therefore agent Shutdown() — would block in join() for up
    // to a full poll tick.
    struct ConfigFileWatcher::StopSignal {
        std::mutex mutex;
        std::condition_variable cv;
        bool requested{false};

        void request() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                requested = true;
            }
            cv.notify_all();
        }

        bool stop_requested() {
            std::lock_guard<std::mutex> lock(mutex);
            return requested;
        }

        // Waits out one poll tick; returns true when a stop was requested
        // (either already pending or arriving during the wait).
        bool wait(std::chrono::milliseconds tick) {
            std::unique_lock<std::mutex> lock(mutex);
            return cv.wait_for(lock, tick, [this] { return requested; });
        }
    };

    constexpr auto kDefaultConfigWatcherPollInterval = std::chrono::milliseconds(1000);

    // Poll interval applied to the NEXT started watcher; each watcher thread
    // captures its value at start, so a running watcher is unaffected.
    // Guarded by config_watcher_mutex().
    static std::chrono::milliseconds& config_watcher_poll_interval() {
        static auto* interval = new std::chrono::milliseconds(kDefaultConfigWatcherPollInterval);
        return *interval;
    }

    // Guards the poll-interval knob above; watcher instance state is guarded
    // by each instance's own mutex_.
    static std::mutex& config_watcher_mutex() {
        static auto* mutex = new std::mutex();
        return *mutex;
    }

    // Reads the whole config file; empty (with an error log) when unreadable.
    static std::string read_config_file(const std::string& config_file_path) {
        if (std::ifstream file(config_file_path); file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }
        LOG_ERROR("can't open config file = {}", config_file_path);
        return {};
    }

    void set_config_watcher_poll_interval(std::chrono::milliseconds interval) {
        std::lock_guard<std::mutex> lock(config_watcher_mutex());
        config_watcher_poll_interval() = interval.count() > 0
            ? interval
            : kDefaultConfigWatcherPollInterval;
    }

    static std::chrono::milliseconds config_watcher_poll_interval_copy() {
        std::lock_guard<std::mutex> lock(config_watcher_mutex());
        return config_watcher_poll_interval();
    }

    ConfigFileWatcher::ConfigFileWatcher(std::string file_path, std::function<void()> reload)
        : file_path_(std::move(file_path)), reload_(std::move(reload)) {}

    ConfigFileWatcher::~ConfigFileWatcher() {
        // stop() joins (or abandons, in a forked child) so the member thread
        // is never destroyed joinable. Failures degrade to abandoning the
        // handle: throwing from a destructor would terminate the host.
        try {
            stop();
        } catch (...) {
            abandon_thread(thread_);
        }
    }

    void ConfigFileWatcher::start() {
        std::lock_guard<std::mutex> lock(mutex_);
        // Non-throwing exists(): a transient filesystem error here must degrade
        // to "no watcher", not propagate into StartAgent()'s catch and leave
        // the whole agent offline.
        std::error_code exists_ec;
        if (file_path_.empty() || !std::filesystem::exists(file_path_, exists_ec)) {
            return;
        }

        if (thread_.joinable()) {
            // A joinable handle from THIS process means the watcher is already
            // running. A joinable handle inherited across fork() references a
            // thread that does not exist in this child — abandon it (never
            // join or detach; see abandon_thread()) and fall through to start
            // a fresh watcher for this process.
            if (owner_pid_ == getpid()) {
                return;
            }
            abandon_thread(thread_);
        }
        auto stop = std::make_shared<StopSignal>();
        stop_ = stop;
        owner_pid_ = getpid();
        // Captured once: the watcher keeps this tick for its lifetime, so a
        // later set_config_watcher_poll_interval() cannot race the running
        // thread.
        const auto tick = config_watcher_poll_interval_copy();

        thread_ = std::thread([path = file_path_, reload = reload_, stop, tick]() {
            // Seed with the non-throwing overload: the throwing form could
            // escape this thread function (the file may have been removed
            // between the exists() check above and the thread starting), and
            // an exception leaving a std::thread calls std::terminate(),
            // crashing the host. On error last_write_time stays default-
            // constructed, so the first iteration just treats the file as
            // changed and attempts a reload.
            std::error_code seed_ec;
            auto last_write_time = std::filesystem::last_write_time(path, seed_ec);

            // wait() covers both a stop pending before the tick and one
            // arriving mid-tick, so shutdown need not wait out the polling
            // interval. The check below avoids starting expensive reload work
            // after an observed stop; a stop can still race that check, which
            // is why stop() joins this thread — the owning agent stops the
            // watcher before tearing anything down, so an in-flight reload
            // always completes against a live agent.
            while (!stop->wait(tick)) {
                try {
                    auto current = std::filesystem::last_write_time(path);
                    if (current != last_write_time) {
                        last_write_time = current;
                        if (!stop->stop_requested()) {
                            reload();
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

    void ConfigFileWatcher::requestStop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            stop_->request();
        }
    }

    void ConfigFileWatcher::stop() {
        std::thread watcher_to_join;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!thread_.joinable()) {
                return;
            }
            // Inherited across fork(): the thread does not exist here, so
            // abandon the dead handle rather than joining it (which would
            // abort) or detaching it (which segfaults on glibc — see
            // abandon_thread()).
            if (owner_pid_ != getpid()) {
                abandon_thread(thread_);
                return;
            }
            if (stop_) {
                stop_->request();
            }
            watcher_to_join = std::move(thread_);
        }
        watcher_to_join.join();
    }

    // yaml-cpp resolves map keys case-sensitively. To let users write config
    // keys in any case (e.g. "collector"/"Collector"/"COLLECTOR", "host"/"Host"),
    // resolve each key by first trying an exact match and then falling back to a
    // case-insensitive scan of the map's keys. Returns the matched value node, or
    // an undefined node when `yaml` is not a map or no key matches — an undefined
    // node is falsy, so callers keep their existing `if (node)` / default-value
    // handling unchanged.
    static YAML::Node find_node(const YAML::Node& yaml, std::string_view cname) {
        if (!yaml || !yaml.IsMap()) {
            return YAML::Node(YAML::NodeType::Undefined);
        }
        // Fast path: an exact match avoids scanning when the key is already
        // cased correctly (the common case).
        if (auto exact = yaml[std::string(cname)]) {
            return exact;
        }
        for (const auto& kv : yaml) {
            if (kv.first.IsScalar() && compare_string(kv.first.Scalar(), cname)) {
                return kv.second;
            }
        }
        return YAML::Node(YAML::NodeType::Undefined);
    }

    // Each getter guards the whole lookup, not just the conversion: yaml-cpp
    // throws from the subscript itself (BadSubscript on scalar nodes) and from
    // element-level conversions (TypedBadConversion<Element> inside vector
    // decoding), all of which derive from YAML::Exception. A malformed config
    // must degrade to defaults, never throw into the embedding application.
    static bool get_boolean(const YAML::Node& yaml, std::string_view cname, bool default_value) {
        try {
            if (auto node = find_node(yaml, cname)) {
                return node.as<bool>();
            }
        } catch (const YAML::Exception& e) {
            LOG_WARN("Failed to read '{}' as boolean: {}. Using default value: {}",
                     std::string(cname), e.what(), default_value);
        }

        return default_value;
    }

    static std::string get_string(const YAML::Node& yaml, std::string_view cname, std::string default_value) {
        try {
            if (auto node = find_node(yaml, cname)) {
                return node.as<std::string>();
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
            if (auto node = find_node(yaml, cname)) {
                return node.as<std::vector<std::string>>();
            }
        } catch (const YAML::Exception& e) {
            LOG_WARN("Failed to read '{}' as string vector: {}. Using default value",
                     std::string(cname), e.what());
        }

        return default_value;
    }

    static int get_int(const YAML::Node& yaml, std::string_view cname, int default_value) {
        try {
            if (auto node = find_node(yaml, cname)) {
                return node.as<int>();
            }
        } catch (const YAML::Exception& e) {
            LOG_WARN("Failed to read '{}' as int: {}. Using default value: {}",
                     std::string(cname), e.what(), default_value);
        }

        return default_value;
    }

    static void load_grpc_channel_yaml(const YAML::Node& grpc, Config::GrpcChannelOptions& options) {
        options.keepalive_time_ms = get_int(grpc, "KeepAliveTimeMs", options.keepalive_time_ms);
        options.keepalive_timeout_ms = get_int(grpc, "KeepAliveTimeoutMs", options.keepalive_timeout_ms);
        options.keepalive_permit_without_calls =
            get_boolean(grpc, "KeepAlivePermitWithoutCalls", options.keepalive_permit_without_calls);
        options.max_send_message_size = get_int(grpc, "MaxSendMessageSize", options.max_send_message_size);
        options.max_receive_message_size = get_int(grpc, "MaxReceiveMessageSize", options.max_receive_message_size);
        options.sender_queue_size = get_int(grpc, "SenderQueueSize", options.sender_queue_size);
    }

    static void load_grpc_yaml(const YAML::Node& collector, Config& config) {
        if (auto grpc = find_node(collector, "Grpc")) {
            config.collector.grpc.ssl.enable = get_boolean(grpc, "SslEnable", config.collector.grpc.ssl.enable);
            config.collector.grpc.ssl.trust_cert_file_path =
                get_string(grpc, "TrustCertFilePath", config.collector.grpc.ssl.trust_cert_file_path);
            config.collector.grpc.ssl.root_cert_file_path =
                get_string(grpc, "RootCertFilePath", config.collector.grpc.ssl.root_cert_file_path);

            load_grpc_channel_yaml(grpc, config.collector.grpc.channel);
        }
    }

    static double get_double(const YAML::Node& yaml, std::string_view cname, double default_value) {
        try {
            if (auto node = find_node(yaml, cname)) {
                return node.as<double>();
            }
        } catch (const YAML::Exception& e) {
            LOG_WARN("Failed to read '{}' as double: {}. Using default value: {}",
                     std::string(cname), e.what(), default_value);
        }

        return default_value;
    }

    // Every getter falls back to the current member value, never a hardcoded
    // default. `config` arrives pre-seeded: with the Config member initializers
    // (the defaults) on a first load, or with a copy of the running config on a
    // reload — so a key absent from the file keeps the running value instead of
    // silently reverting to its default.
    static void load_yaml_config(const YAML::Node& yaml, Config& config, bool& is_container_set) {
        if (yaml.size() < 1) {
            return;
        }

        config.log.level = get_string(yaml, "LogLevel", config.log.level);
        config.enable = get_boolean(yaml, "Enable", config.enable);
        config.app_name_ = get_string(yaml, "ApplicationName", config.app_name_);
        config.agent_id_ = get_string(yaml, "AgentId", config.agent_id_);
        config.agent_name_ = get_string(yaml, "AgentName", config.agent_name_);
        config.uid_version_ = get_string(yaml, "UidVersion", config.uid_version_);
        config.service_name_ = get_string(yaml, "ServiceName", config.service_name_);
        config.api_key_ = get_string(yaml, "ApiKey", config.api_key_);

        if (auto log = find_node(yaml, "Log")) {
            config.log.level = get_string(log, "Level", config.log.level);
            config.log.file_path = get_string(log, "FilePath", config.log.file_path);
            config.log.max_file_size = get_int(log, "MaxFileSize", config.log.max_file_size);
        }

        if (auto collector = find_node(yaml, "Collector")) {
            // Host/AgentPort/SpanPort/StatPort take precedence; the deprecated
            // GrpcHost/GrpcAgentPort/GrpcSpanPort/GrpcStatPort keys are read as
            // a fallback for backward compatibility.
            config.collector.host =
                get_string(collector, "Host", get_string(collector, "GrpcHost", config.collector.host));
            config.collector.agent_port =
                get_int(collector, "AgentPort", get_int(collector, "GrpcAgentPort", config.collector.agent_port));
            config.collector.span_port =
                get_int(collector, "SpanPort", get_int(collector, "GrpcSpanPort", config.collector.span_port));
            config.collector.stat_port =
                get_int(collector, "StatPort", get_int(collector, "GrpcStatPort", config.collector.stat_port));

            if (auto agent_info = find_node(collector, "AgentInfo")) {
                config.collector.agent_info.refresh_interval_ms = get_int(agent_info, "RefreshIntervalMs", config.collector.agent_info.refresh_interval_ms);
                config.collector.agent_info.send_retry_interval_ms = get_int(agent_info, "SendRetryIntervalMs", config.collector.agent_info.send_retry_interval_ms);
                config.collector.agent_info.max_try_per_attempt = get_int(agent_info, "MaxTryPerAttempt", config.collector.agent_info.max_try_per_attempt);
            }
            if (auto span_batch = find_node(collector, "SpanBatch")) {
                config.collector.span_batch.size = get_int(span_batch, "Size", config.collector.span_batch.size);
                config.collector.span_batch.flush_interval_ms = get_int(span_batch, "FlushIntervalMs", config.collector.span_batch.flush_interval_ms);
                config.collector.span_batch.collect_deadline_ms = get_int(span_batch, "CollectDeadlineMs", config.collector.span_batch.collect_deadline_ms);
                config.collector.span_batch.max_concurrent_requests = get_int(span_batch, "MaxConcurrentRequests", config.collector.span_batch.max_concurrent_requests);
            }
            load_grpc_yaml(collector, config);
        }

        if (auto stat = find_node(yaml, "Stat")) {
            config.stat.enable = get_boolean(stat, "Enable", config.stat.enable);
            config.stat.batch_count = get_int(stat, "BatchCount", config.stat.batch_count);
            config.stat.collect_interval = get_int(stat, "BatchInterval", config.stat.collect_interval);
        }

        if (auto http = find_node(yaml, "Http")) {
            config.http.url_stat.enable = get_boolean(http, "CollectUrlStat", config.http.url_stat.enable);
            config.http.url_stat.limit = get_int(http, "UrlStatLimit", config.http.url_stat.limit);
            config.http.url_stat.queue_size = get_int(http, "UrlStatQueueSize", static_cast<int>(config.http.url_stat.queue_size));
            config.http.url_stat.enable_trim_path = get_boolean(http, "UrlStatEnableTrimPath", config.http.url_stat.enable_trim_path);
            config.http.url_stat.trim_path_depth = get_int(http, "UrlStatTrimPathDepth", config.http.url_stat.trim_path_depth);
            config.http.url_stat.method_prefix = get_boolean(http, "UrlStatMethodPrefix", config.http.url_stat.method_prefix);

            if (auto srv = find_node(http, "Server")) {
                config.http.server.status_errors = get_string_vector(srv, "StatusCodeErrors", config.http.server.status_errors);
                config.http.server.exclude_url = get_string_vector(srv, "ExcludeUrl", config.http.server.exclude_url);
                config.http.server.exclude_method = get_string_vector(srv, "ExcludeMethod", config.http.server.exclude_method);
                config.http.server.rec_request_header = get_string_vector(srv, "RecordRequestHeader", config.http.server.rec_request_header);
                config.http.server.rec_request_cookie = get_string_vector(srv, "RecordRequestCookie", config.http.server.rec_request_cookie);
                config.http.server.rec_response_header = get_string_vector(srv, "RecordResponseHeader", config.http.server.rec_response_header);
            }

            if (auto cli = find_node(http, "Client")) {
                config.http.client.rec_request_header = get_string_vector(cli, "RecordRequestHeader", config.http.client.rec_request_header);
                config.http.client.rec_request_cookie = get_string_vector(cli, "RecordRequestCookie", config.http.client.rec_request_cookie);
                config.http.client.rec_response_header = get_string_vector(cli, "RecordResponseHeader", config.http.client.rec_response_header);
            }
        }

        if (auto sampling = find_node(yaml, "Sampling")) {
            config.sampling.type = get_string(sampling, "Type", config.sampling.type);
            config.sampling.counter_rate = get_int(sampling, "CounterRate", config.sampling.counter_rate);
            config.sampling.percent_rate = get_double(sampling, "PercentRate", config.sampling.percent_rate);
            config.sampling.new_throughput = get_int(sampling, "NewThroughput", config.sampling.new_throughput);
            config.sampling.cont_throughput = get_int(sampling, "ContinueThroughput", config.sampling.cont_throughput);
        }

        if (auto span = find_node(yaml, "Span")) {
            config.span.queue_size = get_int(span, "QueueSize", static_cast<int>(config.span.queue_size));
            config.span.max_event_depth = get_int(span, "MaxEventDepth", config.span.max_event_depth);
            config.span.max_event_sequence = get_int(span, "MaxEventSequence", config.span.max_event_sequence);
            config.span.event_chunk_size = get_int(span, "EventChunkSize", static_cast<int>(config.span.event_chunk_size));
        }

        if (auto is_container = find_node(yaml, "IsContainer")) {
            // The key being present (even malformed) means the user decided
            // containerness explicitly, so auto-detection is skipped either way.
            is_container_set = true;
            try {
                config.is_container = is_container.as<bool>();
            } catch (const YAML::Exception& e) {
                LOG_WARN("Failed to read 'IsContainer' as boolean: {}. Using default value: {}",
                         e.what(), config.is_container);
            }
        }

        if (auto sql = find_node(yaml, "Sql")) {
            config.sql.max_bind_args_size = get_int(sql, "MaxBindArgsSize", config.sql.max_bind_args_size);
            config.sql.enable_sql_stats = get_boolean(sql, "EnableSqlStats", config.sql.enable_sql_stats);
            config.sql.enable_raw_sql_cache = get_boolean(sql, "EnableRawSqlCache", config.sql.enable_raw_sql_cache);
            config.sql.trace_bind_value = get_boolean(sql, "TraceBindValue", config.sql.trace_bind_value);
        }

        config.enable_callstack_trace = get_boolean(yaml, "EnableCallstackTrace", config.enable_callstack_trace);
        config.enable_config_file_watcher =
            get_boolean(yaml, "EnableConfigFileWatcher", config.enable_config_file_watcher);
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

    static void load_env_grpc_channel(const std::string& prefix,
                                      Config::GrpcChannelOptions& options,
                                      const char* keepalive_time_env,
                                      const char* keepalive_timeout_env,
                                      const char* keepalive_permit_env,
                                      const char* max_send_env,
                                      const char* max_receive_env,
                                      const char* sender_queue_env) {
        if(auto e = get_env(prefix, keepalive_time_env)) {
            options.keepalive_time_ms = safe_env_stoi(e.name.c_str(), e.value, options.keepalive_time_ms);
        }
        if(auto e = get_env(prefix, keepalive_timeout_env)) {
            options.keepalive_timeout_ms = safe_env_stoi(e.name.c_str(), e.value, options.keepalive_timeout_ms);
        }
        if(auto e = get_env(prefix, keepalive_permit_env)) {
            options.keepalive_permit_without_calls =
                safe_env_stob(e.name.c_str(), e.value, options.keepalive_permit_without_calls);
        }
        if(auto e = get_env(prefix, max_send_env)) {
            options.max_send_message_size = safe_env_stoi(e.name.c_str(), e.value, options.max_send_message_size);
        }
        if(auto e = get_env(prefix, max_receive_env)) {
            options.max_receive_message_size = safe_env_stoi(e.name.c_str(), e.value, options.max_receive_message_size);
        }
        if(auto e = get_env(prefix, sender_queue_env)) {
            options.sender_queue_size = safe_env_stoi(e.name.c_str(), e.value, options.sender_queue_size);
        }
    }

    // Like the yaml getters, every parse failure falls back to the CURRENT
    // member value — which at this point carries the yaml-loaded (or default)
    // setting — never a hardcoded default. A malformed env var must degrade to
    // "env override ignored", not silently clobber a value the user set in the
    // config file.
    static void load_env_config(const std::string& prefix, Config& config, bool& is_container_set) {
        if(auto e = get_env(prefix, env::ENABLE)) {
            config.enable = safe_env_stob(e.name.c_str(), e.value, config.enable);
        }
        if(auto e = get_env(prefix, env::APPLICATION_NAME)) {
            config.app_name_ = std::string(e.value);
        }
        if(auto e = get_env(prefix, env::AGENT_ID)) {
            config.agent_id_ = std::string(e.value);
        }
        if(auto e = get_env(prefix, env::AGENT_NAME)) {
            config.agent_name_ = std::string(e.value);
        }
        if(auto e = get_env(prefix, env::UID_VERSION)) {
            config.uid_version_ = std::string(e.value);
        }
        if(auto e = get_env(prefix, env::SERVICE_NAME)) {
            config.service_name_ = std::string(e.value);
        }
        if(auto e = get_env(prefix, env::API_KEY)) {
            config.api_key_ = std::string(e.value);
        }

        if(auto e = get_env(prefix, env::LOG_LEVEL)) {
            config.log.level = std::string(e.value);
        }
        if(auto e = get_env(prefix, env::LOG_FILE_PATH)) {
            config.log.file_path = std::string(e.value);
        }
        if(auto e = get_env(prefix, env::LOG_MAX_FILE_SIZE)) {
            config.log.max_file_size = safe_env_stoi(e.name.c_str(), e.value, config.log.max_file_size);
        }

        // The deprecated GRPC_* variables are read first, then the preferred
        // COLLECTOR_* variables override them when both are set.
        if(auto e = get_env(prefix, env::GRPC_HOST)) {
            config.collector.host = std::string(e.value);
        }
        if(auto e = get_env(prefix, env::COLLECTOR_HOST)) {
            config.collector.host = std::string(e.value);
        }
        if(auto e = get_env(prefix, env::GRPC_AGENT_PORT)) {
            config.collector.agent_port = safe_env_stoi(e.name.c_str(), e.value, config.collector.agent_port);
        }
        if(auto e = get_env(prefix, env::COLLECTOR_AGENT_PORT)) {
            config.collector.agent_port = safe_env_stoi(e.name.c_str(), e.value, config.collector.agent_port);
        }
        if(auto e = get_env(prefix, env::GRPC_SPAN_PORT)) {
            config.collector.span_port = safe_env_stoi(e.name.c_str(), e.value, config.collector.span_port);
        }
        if(auto e = get_env(prefix, env::COLLECTOR_SPAN_PORT)) {
            config.collector.span_port = safe_env_stoi(e.name.c_str(), e.value, config.collector.span_port);
        }
        if(auto e = get_env(prefix, env::GRPC_STAT_PORT)) {
            config.collector.stat_port = safe_env_stoi(e.name.c_str(), e.value, config.collector.stat_port);
        }
        if(auto e = get_env(prefix, env::COLLECTOR_STAT_PORT)) {
            config.collector.stat_port = safe_env_stoi(e.name.c_str(), e.value, config.collector.stat_port);
        }

        if(auto e = get_env(prefix, env::STAT_ENABLE)) {
            config.stat.enable = safe_env_stob(e.name.c_str(), e.value, config.stat.enable);
        }
        if(auto e = get_env(prefix, env::STAT_BATCH_COUNT)) {
            config.stat.batch_count = safe_env_stoi(e.name.c_str(), e.value, config.stat.batch_count);
        }
        if(auto e = get_env(prefix, env::STAT_BATCH_INTERVAL)) {
            config.stat.collect_interval = safe_env_stoi(e.name.c_str(), e.value, config.stat.collect_interval);
        }

        if(auto e = get_env(prefix, env::SAMPLING_TYPE)) {
            config.sampling.type = std::string(e.value);
        }
        if(auto e = get_env(prefix, env::SAMPLING_COUNTER_RATE)) {
            config.sampling.counter_rate = safe_env_stoi(e.name.c_str(), e.value, config.sampling.counter_rate);
        }
        if(auto e = get_env(prefix, env::SAMPLING_PERCENT_RATE)) {
            config.sampling.percent_rate = safe_env_stod(e.name.c_str(), e.value, config.sampling.percent_rate);
        }
        if(auto e = get_env(prefix, env::SAMPLING_NEW_THROUGHPUT)) {
            config.sampling.new_throughput = safe_env_stoi(e.name.c_str(), e.value, config.sampling.new_throughput);
        }
        if(auto e = get_env(prefix, env::SAMPLING_CONTINUE_THROUGHPUT)) {
            config.sampling.cont_throughput = safe_env_stoi(e.name.c_str(), e.value, config.sampling.cont_throughput);
        }

        if(auto e = get_env(prefix, env::SPAN_QUEUE_SIZE)) {
            config.span.queue_size = safe_env_stoi(e.name.c_str(), e.value, static_cast<int>(config.span.queue_size));
        }
        if(auto e = get_env(prefix, env::SPAN_MAX_EVENT_DEPTH)) {
            config.span.max_event_depth = safe_env_stoi(e.name.c_str(), e.value, config.span.max_event_depth);
        }
        if(auto e = get_env(prefix, env::SPAN_MAX_EVENT_SEQUENCE)) {
            config.span.max_event_sequence = safe_env_stoi(e.name.c_str(), e.value, config.span.max_event_sequence);
        }
        if(auto e = get_env(prefix, env::SPAN_EVENT_CHUNK_SIZE)) {
            config.span.event_chunk_size = safe_env_stoi(e.name.c_str(), e.value, config.span.event_chunk_size);
        }
        if(auto e = get_env(prefix, env::SPAN_BATCH_SIZE)) {
            config.collector.span_batch.size = safe_env_stoi(e.name.c_str(), e.value, config.collector.span_batch.size);
        }
        if(auto e = get_env(prefix, env::SPAN_BATCH_FLUSH_INTERVAL_MS)) {
            config.collector.span_batch.flush_interval_ms = safe_env_stoi(e.name.c_str(), e.value, config.collector.span_batch.flush_interval_ms);
        }
        if(auto e = get_env(prefix, env::SPAN_BATCH_COLLECT_DEADLINE_MS)) {
            config.collector.span_batch.collect_deadline_ms = safe_env_stoi(e.name.c_str(), e.value, config.collector.span_batch.collect_deadline_ms);
        }
        if(auto e = get_env(prefix, env::SPAN_BATCH_MAX_CONCURRENT_REQUESTS)) {
            config.collector.span_batch.max_concurrent_requests = safe_env_stoi(e.name.c_str(), e.value, config.collector.span_batch.max_concurrent_requests);
        }
        if(auto e = get_env(prefix, env::AGENT_INFO_REFRESH_INTERVAL_MS)) {
            config.collector.agent_info.refresh_interval_ms = safe_env_stoi(e.name.c_str(), e.value, config.collector.agent_info.refresh_interval_ms);
        }
        if(auto e = get_env(prefix, env::AGENT_INFO_SEND_RETRY_INTERVAL_MS)) {
            config.collector.agent_info.send_retry_interval_ms = safe_env_stoi(e.name.c_str(), e.value, config.collector.agent_info.send_retry_interval_ms);
        }
        if(auto e = get_env(prefix, env::AGENT_INFO_MAX_TRY_PER_ATTEMPT)) {
            config.collector.agent_info.max_try_per_attempt = safe_env_stoi(e.name.c_str(), e.value, config.collector.agent_info.max_try_per_attempt);
        }

        if(auto e = get_env(prefix, env::GRPC_SSL_ENABLE)) {
            config.collector.grpc.ssl.enable = safe_env_stob(e.name.c_str(), e.value, config.collector.grpc.ssl.enable);
        }
        if(auto e = get_env(prefix, env::GRPC_SSL_TRUST_CERT_FILE_PATH)) {
            config.collector.grpc.ssl.trust_cert_file_path = std::string(e.value);
        }
        if(auto e = get_env(prefix, env::GRPC_SSL_ROOT_CERT_FILE_PATH)) {
            config.collector.grpc.ssl.root_cert_file_path = std::string(e.value);
        }
        load_env_grpc_channel(prefix, config.collector.grpc.channel,
                              env::GRPC_KEEPALIVE_TIME_MS,
                              env::GRPC_KEEPALIVE_TIMEOUT_MS,
                              env::GRPC_KEEPALIVE_PERMIT_WITHOUT_CALLS,
                              env::GRPC_MAX_SEND_MESSAGE_SIZE,
                              env::GRPC_MAX_RECEIVE_MESSAGE_SIZE,
                              env::GRPC_SENDER_QUEUE_SIZE);

        if(auto e = get_env(prefix, env::IS_CONTAINER)) {
            config.is_container = safe_env_stob(e.name.c_str(), e.value, config.is_container);
            is_container_set = true;
        }

        if(auto e = get_env(prefix, env::HTTP_COLLECT_URL_STAT)) {
            config.http.url_stat.enable = safe_env_stob(e.name.c_str(), e.value, config.http.url_stat.enable);
        }
        if(auto e = get_env(prefix, env::HTTP_URL_STAT_LIMIT)) {
            config.http.url_stat.limit = safe_env_stoi(e.name.c_str(), e.value, config.http.url_stat.limit);
        }
        if(auto e = get_env(prefix, env::HTTP_URL_STAT_QUEUE_SIZE)) {
            config.http.url_stat.queue_size = safe_env_stoi(e.name.c_str(), e.value, static_cast<int>(config.http.url_stat.queue_size));
        }
        if(auto e = get_env(prefix, env::HTTP_URL_STAT_ENABLE_TRIM_PATH)) {
            config.http.url_stat.enable_trim_path = safe_env_stob(e.name.c_str(), e.value, config.http.url_stat.enable_trim_path);
        }
        if(auto e = get_env(prefix, env::HTTP_URL_STAT_TRIM_PATH_DEPTH)) {
            config.http.url_stat.trim_path_depth = safe_env_stoi(e.name.c_str(), e.value, config.http.url_stat.trim_path_depth);
        }
        if(auto e = get_env(prefix, env::HTTP_URL_STAT_METHOD_PREFIX)) {
            config.http.url_stat.method_prefix = safe_env_stob(e.name.c_str(), e.value, config.http.url_stat.method_prefix);
        }

        // SkipEmpty keeps an empty (or all-commas) variable from producing
        // phantom "" entries: e.g. HTTP_SERVER_EXCLUDE_URL="" must clear the
        // list, not build a URL filter around a single empty pattern.
        if(auto e = get_env(prefix, env::HTTP_SERVER_STATUS_CODE_ERRORS)) {
            config.http.server.status_errors = absl::StrSplit(e.value, ',', absl::SkipEmpty());
        }
        if(auto e = get_env(prefix, env::HTTP_SERVER_EXCLUDE_URL)) {
            config.http.server.exclude_url = absl::StrSplit(e.value, ',', absl::SkipEmpty());
        }
        if(auto e = get_env(prefix, env::HTTP_SERVER_EXCLUDE_METHOD)) {
            config.http.server.exclude_method = absl::StrSplit(e.value, ',', absl::SkipEmpty());
        }
        if(auto e = get_env(prefix, env::HTTP_SERVER_RECORD_REQUEST_HEADER)) {
            config.http.server.rec_request_header = absl::StrSplit(e.value, ',', absl::SkipEmpty());
        }
        if(auto e = get_env(prefix, env::HTTP_SERVER_RECORD_REQUEST_COOKIE)) {
            config.http.server.rec_request_cookie = absl::StrSplit(e.value, ',', absl::SkipEmpty());
        }
        if(auto e = get_env(prefix, env::HTTP_SERVER_RECORD_RESPONSE_HEADER)) {
            config.http.server.rec_response_header = absl::StrSplit(e.value, ',', absl::SkipEmpty());
        }
        if(auto e = get_env(prefix, env::HTTP_CLIENT_RECORD_REQUEST_HEADER)) {
            config.http.client.rec_request_header = absl::StrSplit(e.value, ',', absl::SkipEmpty());
        }
        if(auto e = get_env(prefix, env::HTTP_CLIENT_RECORD_REQUEST_COOKIE)) {
            config.http.client.rec_request_cookie = absl::StrSplit(e.value, ',', absl::SkipEmpty());
        }
        if(auto e = get_env(prefix, env::HTTP_CLIENT_RECORD_RESPONSE_HEADER)) {
            config.http.client.rec_response_header = absl::StrSplit(e.value, ',', absl::SkipEmpty());
        }

        if(auto e = get_env(prefix, env::SQL_MAX_BIND_ARGS_SIZE)) {
            config.sql.max_bind_args_size = safe_env_stoi(e.name.c_str(), e.value, config.sql.max_bind_args_size);
        }
        if(auto e = get_env(prefix, env::SQL_ENABLE_SQL_STATS)) {
            config.sql.enable_sql_stats = safe_env_stob(e.name.c_str(), e.value, config.sql.enable_sql_stats);
        }
        if(auto e = get_env(prefix, env::SQL_ENABLE_RAW_SQL_CACHE)) {
            config.sql.enable_raw_sql_cache = safe_env_stob(e.name.c_str(), e.value, config.sql.enable_raw_sql_cache);
        }
        if(auto e = get_env(prefix, env::SQL_TRACE_BIND_VALUE)) {
            config.sql.trace_bind_value = safe_env_stob(e.name.c_str(), e.value, config.sql.trace_bind_value);
        }
        if(auto e = get_env(prefix, env::ENABLE_CALLSTACK_TRACE)) {
            config.enable_callstack_trace = safe_env_stob(e.name.c_str(), e.value, config.enable_callstack_trace);
        }
        if(auto e = get_env(prefix, env::ENABLE_CONFIG_FILE_WATCHER)) {
            config.enable_config_file_watcher =
                safe_env_stob(e.name.c_str(), e.value, config.enable_config_file_watcher);
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

    std::string resolve_config_file_path(const AgentOptions& options) {
        if (auto e = get_env(effective_env_prefix(options), env::CONFIG_FILE)) {
            return e.value;
        }
        return options.config_file_path;
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
    constexpr int MIN_URL_STAT_QUEUE_SIZE = 1;
    constexpr int MAX_URL_STAT_QUEUE_SIZE = 65536;

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
    }

    // Appends "-<suffix>" to base, truncating base so the result fits max_len.
    static std::string append_id_suffix(const std::string& base, const std::string& suffix,
                                        size_t max_len) {
        const std::string sep_suffix = "-" + suffix;
        std::string result = base;
        if (result.size() + sep_suffix.size() > max_len && sep_suffix.size() < max_len) {
            result.resize(max_len - sep_suffix.size());
        }
        return result + sep_suffix;
    }

    // Per-worker identity for multi-process hosts (see
    // AgentOptions::instance_suffix). A pinned agent id gets
    // "-<instance_suffix>" appended — or "-<pid>" with a warning when no
    // suffix is configured — so sibling pre-fork workers sharing one
    // configured id register as distinct agent instances. An explicitly
    // configured agent name gets the configured suffix too (display names
    // need not be unique, so the name never falls back to the pid). An
    // auto-generated id is already process-unique and is left untouched, as
    // is an agent name that merely defaulted to the agent id.
    static void apply_instance_suffix(Config& config, const AgentOptions& options,
                                      const std::string& explicit_agent_name) {
        std::string suffix = options.instance_suffix;
        if (!suffix.empty() && !validate_id(suffix, object_name::AGENT_ID_MAX_LEN)) {
            LOG_WARN("invalid instance_suffix '{}' (allowed: [a-zA-Z0-9._-], max {} chars); ignoring it",
                     suffix, object_name::AGENT_ID_MAX_LEN);
            suffix.clear();
        }

        if (config.agent_id_pinned_) {
            const std::string old_id = config.agent_id_;
            if (!suffix.empty()) {
                config.agent_id_ = append_id_suffix(old_id, suffix, object_name::AGENT_ID_MAX_LEN);
                LOG_INFO("per-worker AgentId: '{}' -> '{}'", old_id, config.agent_id_);
            } else {
                config.agent_id_ = append_id_suffix(old_id,
                                                    std::to_string(static_cast<long>(getpid())),
                                                    object_name::AGENT_ID_MAX_LEN);
                LOG_WARN("pinned AgentId '{}' got a pid suffix ('{}'): sibling pre-fork workers "
                         "must not share one agent id; set AgentOptions::instance_suffix for a "
                         "stable per-worker id",
                         old_id, config.agent_id_);
            }
        }

        if (!suffix.empty() && !explicit_agent_name.empty()) {
            const size_t name_max_len = config.is_v4() ? object_name::AGENT_NAME_MAX_LEN_V4
                                                       : object_name::AGENT_NAME_MAX_LEN;
            config.agent_name_ = append_id_suffix(explicit_agent_name, suffix, name_max_len);
        }
    }

    // Expands per-worker placeholders in the configured log file path at
    // sink-application time (Config keeps the raw value, so reload
    // comparisons and to_config_string() round-trips stay stable):
    //   %pid%    -> current process id
    //   %suffix% -> AgentOptions::instance_suffix, or the pid when unset
    // Sibling pre-fork workers can thereby write separate log files — the
    // built-in size rotation is not multi-process safe on a shared file.
    static std::string expand_log_file_path(const std::string& path,
                                            const AgentOptions& options) {
        if (path.find('%') == std::string::npos) {
            return path;
        }
        const std::string pid = std::to_string(static_cast<long>(getpid()));
        const std::string& suffix =
            options.instance_suffix.empty() ? pid : options.instance_suffix;
        return absl::StrReplaceAll(path, {{"%pid%", pid}, {"%suffix%", suffix}});
    }

    static void apply_log_config(const Config& cfg, const Config* old,
                                 const AgentOptions& options) {
        // Skip the file logger when its settings are unchanged: setFileLogger()
        // closes and reopens the stream, and a reload triggered by an unrelated
        // setting should not churn the log file. An empty path is applied too —
        // that is how removing FilePath at runtime switches back to stdout.
        // Comparing the raw (unexpanded) paths is equivalent: the placeholder
        // expansion is constant within one process and agent.
        if (!old || old->log.file_path != cfg.log.file_path ||
            old->log.max_file_size != cfg.log.max_file_size) {
            Logger::getInstance().setFileLogger(
                expand_log_file_path(cfg.log.file_path, options), cfg.log.max_file_size);
        }
        if (!old || old->log.level != cfg.log.level) {
            Logger::getInstance().setLogLevel(cfg.log.level);
        }
    }

    // make_config() is reached from the public StartAgent() entry point, so
    // it must never let a parsing problem escape into the host application:
    // yaml errors degrade to defaults, and the function-level handler below is
    // the last-resort backstop.
    //
    // `old` is the running agent's config when the sources are re-read for a
    // reload, nullptr on the first load. The returned config is final: on a
    // reload the non-reloadable fields are already retained from `old` and the
    // logger is already reconfigured, so callers pass it straight to
    // reloadConfig().
    std::shared_ptr<Config> make_config(const AgentOptions& options,
                                        const std::shared_ptr<const Config>& old) try {
        // Seed the config with the running values on a reload so every setting
        // absent from the file keeps its current value (including env-sourced
        // ones applied at first load) instead of reverting to its default.
        // On a first load the Config member initializers are the defaults.
        auto config = old ? std::make_shared<Config>(*old) : std::make_shared<Config>();
        bool is_container_set = false;
        const auto prefix = effective_env_prefix(options);

        // File-over-string precedence, documented at AgentOptions: the file's
        // content replaces options.config_yaml wholesale (the sources are
        // never merged), and the path may come from the CONFIG_FILE env var
        // rather than from the embedder itself.
        const auto config_path = resolve_config_file_path(options);
        const std::string user_config =
            !config_path.empty() ? read_config_file(config_path) : options.config_yaml;

        YAML::Node yaml;
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
        // are only meant to seed the very first configuration. When rebuilding
        // for a reload (`old` set), env overrides must not be re-applied —
        // otherwise they would silently override values the user just changed
        // in the config file. Env-sourced values still survive reloads through
        // the old-config seeding above, as long as the file does not
        // explicitly override them.
        if (!old) {
            load_env_config(prefix, *config, is_container_set);
        }

        // Configure the logger immediately on the first load so the rest of
        // make_config() (identity resolution, range checks) already logs to
        // the configured sink and level. On a reload the logger is applied at
        // the end instead, once this config is complete and can no longer be
        // rejected.
        if (!old) {
            apply_log_config(*config, nullptr, options);
        }

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
                // Record whether the id was pinned (explicitly provided and kept
                // as-is) versus auto-generated. v4 always regenerates the id,
                // so it is never pinned. apply_instance_suffix() below uses
                // this to derive a per-worker id in multi-process hosts.
                config->agent_id_pinned_ = (name_version != NameVersion::kV4)
                    && !in.agent_id.empty()
                    && object_name->agent_id == in.agent_id;
                config->agent_id_ = object_name->agent_id;
                config->agent_name_ = object_name->agent_name;
                config->app_name_ = object_name->application_name;
                config->service_name_ = object_name->service_name;
                config->api_key_ = object_name->api_key;
                config->identity_resolved_ = true;

                // First load only: a reload retains the running identity via
                // retainNonReloadableFrom(), so re-applying the suffix there
                // would only produce noise before being overwritten.
                if (!old) {
                    const std::string explicit_agent_name =
                        (!in.agent_name.empty() && object_name->agent_name == in.agent_name)
                            ? in.agent_name
                            : std::string();
                    apply_instance_suffix(*config, options, explicit_agent_name);
                }
            } else {
                // A required identity value is missing/invalid. Keep returning a
                // populated config (callers may inspect it); Config::check() fails
                // and StartAgent() degrades to a noop agent. Ensure agent_id is
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

        if (config->collector.span_batch.size < 1) {
            LOG_WARN("span batch size {} is invalid, using default: {}",
                     config->collector.span_batch.size, defaults::SPAN_BATCH_SIZE);
            config->collector.span_batch.size = defaults::SPAN_BATCH_SIZE;
        }
        if (config->collector.span_batch.flush_interval_ms < 1) {
            LOG_WARN("span batch flush interval {}ms is invalid, using default: {}ms",
                     config->collector.span_batch.flush_interval_ms, defaults::SPAN_BATCH_FLUSH_INTERVAL_MS);
            config->collector.span_batch.flush_interval_ms = defaults::SPAN_BATCH_FLUSH_INTERVAL_MS;
        }
        if (config->collector.span_batch.collect_deadline_ms < 0) {
            LOG_WARN("span batch collect deadline {}ms is invalid, using default: {}ms",
                     config->collector.span_batch.collect_deadline_ms, defaults::SPAN_BATCH_COLLECT_DEADLINE_MS);
            config->collector.span_batch.collect_deadline_ms = defaults::SPAN_BATCH_COLLECT_DEADLINE_MS;
        }
        if (config->collector.span_batch.max_concurrent_requests < 1) {
            LOG_WARN("span batch max concurrent requests {} is invalid, using default: {}",
                     config->collector.span_batch.max_concurrent_requests, defaults::SPAN_BATCH_MAX_CONCURRENT_REQUESTS);
            config->collector.span_batch.max_concurrent_requests = defaults::SPAN_BATCH_MAX_CONCURRENT_REQUESTS;
        }
        if (config->collector.agent_info.refresh_interval_ms < 1) {
            LOG_WARN("agent info refresh interval {}ms is invalid, using default: {}ms",
                     config->collector.agent_info.refresh_interval_ms, defaults::AGENT_INFO_REFRESH_INTERVAL_MS);
            config->collector.agent_info.refresh_interval_ms = defaults::AGENT_INFO_REFRESH_INTERVAL_MS;
        }
        if (config->collector.agent_info.send_retry_interval_ms < 1) {
            LOG_WARN("agent info send retry interval {}ms is invalid, using default: {}ms",
                     config->collector.agent_info.send_retry_interval_ms, defaults::AGENT_INFO_SEND_RETRY_INTERVAL_MS);
            config->collector.agent_info.send_retry_interval_ms = defaults::AGENT_INFO_SEND_RETRY_INTERVAL_MS;
        }
        if (config->collector.agent_info.max_try_per_attempt < 1) {
            LOG_WARN("agent info max try per attempt {} is invalid, using default: {}",
                     config->collector.agent_info.max_try_per_attempt, defaults::AGENT_INFO_MAX_TRY_PER_ATTEMPT);
            config->collector.agent_info.max_try_per_attempt = defaults::AGENT_INFO_MAX_TRY_PER_ATTEMPT;
        }

        if (config->sql.max_bind_args_size < 0) {
            LOG_WARN("sql max bind args size {} is invalid, clamping to 0",
                     config->sql.max_bind_args_size);
            config->sql.max_bind_args_size = 0;
        }

        // A negative limit would cast to a huge size_t at the use site
        // (UrlStatSnapshot::add), disabling the cap and letting the URL map grow
        // unbounded with cardinality. Reject it.
        if (config->http.url_stat.limit < 0) {
            LOG_WARN("http url stat limit {} is invalid, using default: {}",
                     config->http.url_stat.limit, defaults::HTTP_URL_STAT_LIMIT);
            config->http.url_stat.limit = defaults::HTTP_URL_STAT_LIMIT;
        }

        // A negative value would wrap into a huge size_t here, so the upper
        // bound also rejects it.
        if (config->http.url_stat.queue_size < MIN_URL_STAT_QUEUE_SIZE ||
            config->http.url_stat.queue_size > MAX_URL_STAT_QUEUE_SIZE) {
            LOG_WARN("http url stat queue size {} is out of range ({}-{}), using default: {}",
                     config->http.url_stat.queue_size, MIN_URL_STAT_QUEUE_SIZE, MAX_URL_STAT_QUEUE_SIZE,
                     defaults::HTTP_URL_STAT_QUEUE_SIZE);
            config->http.url_stat.queue_size = defaults::HTTP_URL_STAT_QUEUE_SIZE;
        }

        validate_grpc_channel(config->collector.grpc.channel, "grpc", Config::GrpcChannelOptions());

        // Auto-detect only on the first load. On a reload the value is already
        // seeded from the running config (env- or file-sourced) at the top of
        // make_config(); re-running is_container_env() here would clobber an
        // env-set value that the file does not explicitly override. is_container
        // is reloadable and is NOT restored by retainNonReloadableFrom(), so
        // that clobber would otherwise persist across the reload.
        if (!old && !is_container_set) {
            config->is_container = is_container_env();
        }

        if (old) {
            // Finalize the reload config: non-reloadable fields cannot change
            // on a live agent, so retain the running values (with a warning on
            // any attempted change), then reconfigure the logger for the log
            // settings that actually changed. The "config:" line below already
            // goes to the new sink/level and shows the final merged config.
            config->retainNonReloadableFrom(old);
            apply_log_config(*config, old.get(), options);
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
        add_non_default_config(config_strings, "Collector.Grpc.TrustCertFilePath", config.collector.grpc.ssl.trust_cert_file_path,
                               default_config.collector.grpc.ssl.trust_cert_file_path);
        add_non_default_config(config_strings, "Collector.Grpc.RootCertFilePath", config.collector.grpc.ssl.root_cert_file_path,
                               default_config.collector.grpc.ssl.root_cert_file_path);
        add_non_default_config(config_strings, "Collector.Grpc.SslEnable", config.collector.grpc.ssl.enable,
                               default_config.collector.grpc.ssl.enable);
        add_non_default_config(config_strings, "Collector.Grpc.KeepAliveTimeMs", config.collector.grpc.channel.keepalive_time_ms,
                               default_config.collector.grpc.channel.keepalive_time_ms);
        add_non_default_config(config_strings, "Collector.Grpc.KeepAliveTimeoutMs", config.collector.grpc.channel.keepalive_timeout_ms,
                               default_config.collector.grpc.channel.keepalive_timeout_ms);
        add_non_default_config(config_strings, "Collector.Grpc.KeepAlivePermitWithoutCalls",
                               config.collector.grpc.channel.keepalive_permit_without_calls,
                               default_config.collector.grpc.channel.keepalive_permit_without_calls);
        add_non_default_config(config_strings, "Collector.Grpc.MaxSendMessageSize", config.collector.grpc.channel.max_send_message_size,
                               default_config.collector.grpc.channel.max_send_message_size);
        add_non_default_config(config_strings, "Collector.Grpc.MaxReceiveMessageSize", config.collector.grpc.channel.max_receive_message_size,
                               default_config.collector.grpc.channel.max_receive_message_size);
        add_non_default_config(config_strings, "Collector.Grpc.SenderQueueSize", config.collector.grpc.channel.sender_queue_size,
                               default_config.collector.grpc.channel.sender_queue_size);
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
        add_non_default_config(config_strings, "Collector.SpanBatch.Size", config.collector.span_batch.size,
                               default_config.collector.span_batch.size);
        add_non_default_config(config_strings, "Collector.SpanBatch.FlushIntervalMs", config.collector.span_batch.flush_interval_ms,
                               default_config.collector.span_batch.flush_interval_ms);
        add_non_default_config(config_strings, "Collector.SpanBatch.CollectDeadlineMs", config.collector.span_batch.collect_deadline_ms,
                               default_config.collector.span_batch.collect_deadline_ms);
        add_non_default_config(config_strings, "Collector.SpanBatch.MaxConcurrentRequests",
                               config.collector.span_batch.max_concurrent_requests,
                               default_config.collector.span_batch.max_concurrent_requests);
        add_non_default_config(config_strings, "Collector.AgentInfo.RefreshIntervalMs", config.collector.agent_info.refresh_interval_ms,
                               default_config.collector.agent_info.refresh_interval_ms);
        add_non_default_config(config_strings, "Collector.AgentInfo.SendRetryIntervalMs", config.collector.agent_info.send_retry_interval_ms,
                               default_config.collector.agent_info.send_retry_interval_ms);
        add_non_default_config(config_strings, "Collector.AgentInfo.MaxTryPerAttempt", config.collector.agent_info.max_try_per_attempt,
                               default_config.collector.agent_info.max_try_per_attempt);
        add_non_default_config(config_strings, "Http.CollectUrlStat", config.http.url_stat.enable,
                               default_config.http.url_stat.enable);
        add_non_default_config(config_strings, "Http.UrlStatLimit", config.http.url_stat.limit,
                               default_config.http.url_stat.limit);
        add_non_default_config(config_strings, "Http.UrlStatQueueSize", config.http.url_stat.queue_size,
                               default_config.http.url_stat.queue_size);
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
        add_non_default_config(config_strings, "Sql.EnableRawSqlCache", config.sql.enable_raw_sql_cache,
                               default_config.sql.enable_raw_sql_cache);
        add_non_default_config(config_strings, "Sql.TraceBindValue", config.sql.trace_bind_value,
                               default_config.sql.trace_bind_value);
        add_non_default_config(config_strings, "EnableCallstackTrace", config.enable_callstack_trace,
                               default_config.enable_callstack_trace);
        add_non_default_config(config_strings, "EnableConfigFileWatcher", config.enable_config_file_watcher,
                               default_config.enable_config_file_watcher);

        return config_strings;
    }

    std::string to_config_string(const Config& config) {
        YAML::Emitter emitter;

        emitter << YAML::BeginMap;
        emitter << YAML::Key << "ApplicationName" << YAML::Value << config.app_name_;
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
        emitter << YAML::Key << "Host" << YAML::Value << config.collector.host;
        emitter << YAML::Key << "AgentPort" << YAML::Value << config.collector.agent_port;
        emitter << YAML::Key << "SpanPort" << YAML::Value << config.collector.span_port;
        emitter << YAML::Key << "StatPort" << YAML::Value << config.collector.stat_port;

        auto emit_grpc_channel = [&emitter](const Config::GrpcChannelOptions& options) {
            emitter << YAML::Key << "KeepAliveTimeMs" << YAML::Value << options.keepalive_time_ms;
            emitter << YAML::Key << "KeepAliveTimeoutMs" << YAML::Value << options.keepalive_timeout_ms;
            emitter << YAML::Key << "KeepAlivePermitWithoutCalls" << YAML::Value << options.keepalive_permit_without_calls;
            emitter << YAML::Key << "MaxSendMessageSize" << YAML::Value << options.max_send_message_size;
            emitter << YAML::Key << "MaxReceiveMessageSize" << YAML::Value << options.max_receive_message_size;
            emitter << YAML::Key << "SenderQueueSize" << YAML::Value << options.sender_queue_size;
        };

        emitter << YAML::Key << "Grpc";
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "SslEnable" << YAML::Value << config.collector.grpc.ssl.enable;
        emitter << YAML::Key << "TrustCertFilePath" << YAML::Value << config.collector.grpc.ssl.trust_cert_file_path;
        emitter << YAML::Key << "RootCertFilePath" << YAML::Value << config.collector.grpc.ssl.root_cert_file_path;
        emit_grpc_channel(config.collector.grpc.channel);
        emitter << YAML::EndMap;
        emitter << YAML::Key << "AgentInfo";
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "RefreshIntervalMs" << YAML::Value << config.collector.agent_info.refresh_interval_ms;
        emitter << YAML::Key << "SendRetryIntervalMs" << YAML::Value << config.collector.agent_info.send_retry_interval_ms;
        emitter << YAML::Key << "MaxTryPerAttempt" << YAML::Value << config.collector.agent_info.max_try_per_attempt;
        emitter << YAML::EndMap;
        emitter << YAML::Key << "SpanBatch";
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "Size" << YAML::Value << config.collector.span_batch.size;
        emitter << YAML::Key << "FlushIntervalMs" << YAML::Value << config.collector.span_batch.flush_interval_ms;
        emitter << YAML::Key << "CollectDeadlineMs" << YAML::Value << config.collector.span_batch.collect_deadline_ms;
        emitter << YAML::Key << "MaxConcurrentRequests" << YAML::Value << config.collector.span_batch.max_concurrent_requests;
        emitter << YAML::EndMap;
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
        emitter << YAML::EndMap;

        emitter << YAML::Key << "Http";
        emitter << YAML::BeginMap;

        emitter << YAML::Key << "CollectUrlStat" << YAML::Value << config.http.url_stat.enable;
        emitter << YAML::Key << "UrlStatLimit" << YAML::Value << config.http.url_stat.limit;
        emitter << YAML::Key << "UrlStatQueueSize" << YAML::Value << config.http.url_stat.queue_size;
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
        emitter << YAML::Key << "EnableRawSqlCache" << YAML::Value << config.sql.enable_raw_sql_cache;
        emitter << YAML::Key << "TraceBindValue" << YAML::Value << config.sql.trace_bind_value;
        emitter << YAML::EndMap;

        emitter << YAML::Key << "EnableCallstackTrace" << YAML::Value << config.enable_callstack_trace;
        emitter << YAML::Key << "EnableConfigFileWatcher" << YAML::Value << config.enable_config_file_watcher;
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
        return std::tie(lhs.keepalive_time_ms,
                        lhs.keepalive_timeout_ms,
                        lhs.keepalive_permit_without_calls,
                        lhs.max_send_message_size,
                        lhs.max_receive_message_size,
                        lhs.sender_queue_size) ==
               std::tie(rhs.keepalive_time_ms,
                        rhs.keepalive_timeout_ms,
                        rhs.keepalive_permit_without_calls,
                        rhs.max_send_message_size,
                        rhs.max_receive_message_size,
                        rhs.sender_queue_size);
    }

    static bool same_grpc_config(const Config& lhs, const Config& rhs) {
        return std::tie(lhs.collector.grpc.ssl.enable,
                        lhs.collector.grpc.ssl.trust_cert_file_path,
                        lhs.collector.grpc.ssl.root_cert_file_path) ==
               std::tie(rhs.collector.grpc.ssl.enable,
                        rhs.collector.grpc.ssl.trust_cert_file_path,
                        rhs.collector.grpc.ssl.root_cert_file_path) &&
               same_grpc_channel(lhs.collector.grpc.channel, rhs.collector.grpc.channel);
    }

    // Every setting under `collector` is non-reloadable: the endpoint, gRPC
    // transport, agent-info refresh and span-batch tuning are all wired into the
    // running gRPC connection/streams at startup.
    static bool same_collector_config(const Config& lhs, const Config& rhs) {
        return std::tie(lhs.collector.host,
                        lhs.collector.agent_port,
                        lhs.collector.span_port,
                        lhs.collector.stat_port,
                        lhs.collector.agent_info.refresh_interval_ms,
                        lhs.collector.agent_info.send_retry_interval_ms,
                        lhs.collector.agent_info.max_try_per_attempt,
                        lhs.collector.span_batch.size,
                        lhs.collector.span_batch.flush_interval_ms,
                        lhs.collector.span_batch.collect_deadline_ms,
                        lhs.collector.span_batch.max_concurrent_requests) ==
               std::tie(rhs.collector.host,
                        rhs.collector.agent_port,
                        rhs.collector.span_port,
                        rhs.collector.stat_port,
                        rhs.collector.agent_info.refresh_interval_ms,
                        rhs.collector.agent_info.send_retry_interval_ms,
                        rhs.collector.agent_info.max_try_per_attempt,
                        rhs.collector.span_batch.size,
                        rhs.collector.span_batch.flush_interval_ms,
                        rhs.collector.span_batch.collect_deadline_ms,
                        rhs.collector.span_batch.max_concurrent_requests) &&
               same_grpc_config(lhs, rhs);
    }

    static bool same_stat_config(const Config& lhs, const Config& rhs) {
        return std::tie(lhs.stat.enable, lhs.stat.batch_count, lhs.stat.collect_interval) ==
               std::tie(rhs.stat.enable, rhs.stat.batch_count, rhs.stat.collect_interval);
    }

    static bool same_url_stat_config(const Config& lhs, const Config& rhs) {
        return std::tie(lhs.http.url_stat.enable,
                        lhs.http.url_stat.limit,
                        lhs.http.url_stat.queue_size,
                        lhs.http.url_stat.enable_trim_path,
                        lhs.http.url_stat.trim_path_depth,
                        lhs.http.url_stat.method_prefix) ==
               std::tie(rhs.http.url_stat.enable,
                        rhs.http.url_stat.limit,
                        rhs.http.url_stat.queue_size,
                        rhs.http.url_stat.enable_trim_path,
                        rhs.http.url_stat.trim_path_depth,
                        rhs.http.url_stat.method_prefix);
    }

    bool Config::isReloadable(const std::shared_ptr<const Config>& old) const {
        if (!old) return true;
        return std::tie(app_name_, agent_id_, agent_name_,
                        uid_version_, service_name_, api_key_, object_name_version_,
                        span.queue_size, enable_config_file_watcher) ==
               std::tie(old->app_name_, old->agent_id_, old->agent_name_,
                        old->uid_version_, old->service_name_, old->api_key_, old->object_name_version_,
                        old->span.queue_size, old->enable_config_file_watcher) &&
               same_collector_config(*this, *old) &&
               same_stat_config(*this, *old) &&
               same_url_stat_config(*this, *old);
    }

    void Config::retainNonReloadableFrom(const std::shared_ptr<const Config>& old) {
        if (!old) {
            return;
        }

        // isReloadable() returns true only when every non-reloadable field
        // already matches, so a false result means the incoming config tried to
        // change at least one of them. We cannot honor that on a live agent, so
        // warn and fall through to overwrite them with the running values.
        if (!isReloadable(old)) {
            LOG_WARN("non-reloadable config fields changed at runtime "
                     "(identity, collector, stat, http url_stat, span queue "
                     "size or config-file watcher); retaining the existing "
                     "values and reloading the rest");
        }

        app_name_ = old->app_name_;
        agent_id_ = old->agent_id_;
        agent_name_ = old->agent_name_;
        uid_version_ = old->uid_version_;
        service_name_ = old->service_name_;
        api_key_ = old->api_key_;
        object_name_version_ = old->object_name_version_;
        // Identity was resolved for the running agent; keep that outcome so a
        // reload whose new identity failed resolution still passes check().
        identity_resolved_ = old->identity_resolved_;
        collector = old->collector;
        stat = old->stat;
        http.url_stat = old->http.url_stat;
        span.queue_size = old->span.queue_size;
        // Consumed once by Start(): the watcher either was or was not
        // installed, and a reload (running on the watcher's own thread)
        // cannot change that.
        enable_config_file_watcher = old->enable_config_file_watcher;
    }
}
