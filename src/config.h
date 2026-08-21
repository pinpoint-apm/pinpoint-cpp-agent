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

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/types.h>
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
        constexpr int SPAN_BATCH_SIZE = 20;
        constexpr int SPAN_BATCH_FLUSH_INTERVAL_MS = 1000;
        constexpr int SPAN_BATCH_COLLECT_DEADLINE_MS = 500;
        constexpr int SPAN_BATCH_MAX_CONCURRENT_REQUESTS = 10;
        constexpr int AGENT_INFO_REFRESH_INTERVAL_MS = 24 * 60 * 60 * 1000;
        constexpr int AGENT_INFO_SEND_RETRY_INTERVAL_MS = 3000;
        constexpr int AGENT_INFO_MAX_TRY_PER_ATTEMPT = 3;
        constexpr int GRPC_KEEPALIVE_TIME_MS = 30 * 1000;
        constexpr int GRPC_KEEPALIVE_TIMEOUT_MS = 60 * 1000;
        constexpr int GRPC_MAX_MESSAGE_SIZE = 4 * 1024 * 1024;
        constexpr int GRPC_SENDER_QUEUE_SIZE = 1000;
        constexpr int HTTP_URL_STAT_LIMIT = 1024;
        constexpr int HTTP_URL_STAT_QUEUE_SIZE = 1024;
        constexpr int SQL_MAX_BIND_ARGS_SIZE = 1024;
        constexpr int LOG_MAX_FILE_SIZE_MB = 10;
        constexpr const char* LOG_LEVEL = "info";

        constexpr int32_t SPAN_SERVICE_TYPE = SERVICE_TYPE_CPP;
        constexpr int32_t SPAN_EVENT_SERVICE_TYPE = SERVICE_TYPE_CPP_FUNC;
    }

    /**
     * @brief Environment variable name suffixes that override config values.
     *
     * Suffix only: the full name is built as `<prefix>_<suffix>`, the prefix
     * being `DEFAULT_PREFIX` (`PINPOINT_CPP`) unless `AgentOptions::env_prefix`
     * overrides it — e.g. `PINPOINT_CPP_APPLICATION_NAME`.
     */
    namespace env {
        constexpr const char* DEFAULT_PREFIX = "PINPOINT_CPP";
        constexpr const char* ENABLE = "ENABLE";
        constexpr const char* APPLICATION_NAME = "APPLICATION_NAME";
        constexpr const char* AGENT_NAME = "AGENT_NAME";
        constexpr const char* UID_VERSION = "UID_VERSION";
        constexpr const char* SERVICE_NAME = "SERVICE_NAME";
        constexpr const char* API_KEY = "API_KEY";
        constexpr const char* LOG_LEVEL = "LOG_LEVEL";
        constexpr const char* LOG_FILE_PATH = "LOG_FILE_PATH";
        constexpr const char* LOG_MAX_FILE_SIZE = "LOG_MAX_FILE_SIZE";
        constexpr const char* COLLECTOR_HOST = "COLLECTOR_HOST";
        constexpr const char* COLLECTOR_AGENT_PORT = "COLLECTOR_AGENT_PORT";
        constexpr const char* COLLECTOR_SPAN_PORT = "COLLECTOR_SPAN_PORT";
        constexpr const char* COLLECTOR_STAT_PORT = "COLLECTOR_STAT_PORT";
        // Deprecated: use COLLECTOR_HOST / COLLECTOR_*_PORT instead. Kept for
        // backward compatibility; still honored as a fallback when the
        // COLLECTOR_* variant is not set.
        constexpr const char* GRPC_HOST = "GRPC_HOST";
        constexpr const char* GRPC_AGENT_PORT = "GRPC_AGENT_PORT";
        constexpr const char* GRPC_SPAN_PORT = "GRPC_SPAN_PORT";
        constexpr const char* GRPC_STAT_PORT = "GRPC_STAT_PORT";
        constexpr const char* STAT_ENABLE = "STAT_ENABLE";
        constexpr const char* STAT_BATCH_COUNT = "STAT_BATCH_COUNT";
        constexpr const char* STAT_BATCH_INTERVAL = "STAT_BATCH_INTERVAL";
        constexpr const char* SAMPLING_TYPE = "SAMPLING_TYPE";
        constexpr const char* SAMPLING_COUNTER_RATE = "SAMPLING_COUNTER_RATE";
        constexpr const char* SAMPLING_PERCENT_RATE = "SAMPLING_PERCENT_RATE";
        constexpr const char* SAMPLING_NEW_THROUGHPUT = "SAMPLING_NEW_THROUGHPUT";
        constexpr const char* SAMPLING_CONTINUE_THROUGHPUT = "SAMPLING_CONTINUE_THROUGHPUT";
        constexpr const char* SPAN_QUEUE_SIZE = "SPAN_QUEUE_SIZE";
        constexpr const char* SPAN_MAX_EVENT_DEPTH = "SPAN_MAX_EVENT_DEPTH";
        constexpr const char* SPAN_MAX_EVENT_SEQUENCE = "SPAN_MAX_EVENT_SEQUENCE";
        constexpr const char* SPAN_EVENT_CHUNK_SIZE = "SPAN_EVENT_CHUNK_SIZE";
        constexpr const char* SPAN_BATCH_SIZE = "SPAN_BATCH_SIZE";
        constexpr const char* SPAN_BATCH_FLUSH_INTERVAL_MS = "SPAN_BATCH_FLUSH_INTERVAL_MS";
        constexpr const char* SPAN_BATCH_COLLECT_DEADLINE_MS = "SPAN_BATCH_COLLECT_DEADLINE_MS";
        constexpr const char* SPAN_BATCH_MAX_CONCURRENT_REQUESTS = "SPAN_BATCH_MAX_CONCURRENT_REQUESTS";
        constexpr const char* AGENT_INFO_REFRESH_INTERVAL_MS = "AGENT_INFO_REFRESH_INTERVAL_MS";
        constexpr const char* AGENT_INFO_SEND_RETRY_INTERVAL_MS = "AGENT_INFO_SEND_RETRY_INTERVAL_MS";
        constexpr const char* AGENT_INFO_MAX_TRY_PER_ATTEMPT = "AGENT_INFO_MAX_TRY_PER_ATTEMPT";
        constexpr const char* GRPC_SSL_TRUST_CERT_FILE_PATH = "GRPC_SSL_TRUST_CERT_FILE_PATH";
        constexpr const char* GRPC_SSL_ROOT_CERT_FILE_PATH = "GRPC_SSL_ROOT_CERT_FILE_PATH";
        constexpr const char* GRPC_SSL_ENABLE = "GRPC_SSL_ENABLE";
        constexpr const char* GRPC_KEEPALIVE_TIME_MS = "GRPC_KEEPALIVE_TIME_MS";
        constexpr const char* GRPC_KEEPALIVE_TIMEOUT_MS = "GRPC_KEEPALIVE_TIMEOUT_MS";
        constexpr const char* GRPC_KEEPALIVE_PERMIT_WITHOUT_CALLS = "GRPC_KEEPALIVE_PERMIT_WITHOUT_CALLS";
        constexpr const char* GRPC_MAX_SEND_MESSAGE_SIZE = "GRPC_MAX_SEND_MESSAGE_SIZE";
        constexpr const char* GRPC_MAX_RECEIVE_MESSAGE_SIZE = "GRPC_MAX_RECEIVE_MESSAGE_SIZE";
        constexpr const char* GRPC_SENDER_QUEUE_SIZE = "GRPC_SENDER_QUEUE_SIZE";
        constexpr const char* IS_CONTAINER = "IS_CONTAINER";
        constexpr const char* HTTP_COLLECT_URL_STAT = "HTTP_COLLECT_URL_STAT";
        constexpr const char* HTTP_URL_STAT_LIMIT = "HTTP_URL_STAT_LIMIT";
        constexpr const char* HTTP_URL_STAT_QUEUE_SIZE = "HTTP_URL_STAT_QUEUE_SIZE";
        constexpr const char* HTTP_URL_STAT_ENABLE_TRIM_PATH = "HTTP_URL_STAT_ENABLE_TRIM_PATH";
        constexpr const char* HTTP_URL_STAT_TRIM_PATH_DEPTH = "HTTP_URL_STAT_TRIM_PATH_DEPTH";
        constexpr const char* HTTP_URL_STAT_METHOD_PREFIX = "HTTP_URL_STAT_METHOD_PREFIX";
        constexpr const char* HTTP_SERVER_STATUS_CODE_ERRORS = "HTTP_SERVER_STATUS_CODE_ERRORS";
        constexpr const char* HTTP_SERVER_EXCLUDE_URL = "HTTP_SERVER_EXCLUDE_URL";
        constexpr const char* HTTP_SERVER_EXCLUDE_METHOD = "HTTP_SERVER_EXCLUDE_METHOD";
        constexpr const char* HTTP_SERVER_RECORD_REQUEST_HEADER = "HTTP_SERVER_RECORD_REQUEST_HEADER";
        constexpr const char* HTTP_SERVER_RECORD_REQUEST_COOKIE = "HTTP_SERVER_RECORD_REQUEST_COOKIE";
        constexpr const char* HTTP_SERVER_RECORD_RESPONSE_HEADER = "HTTP_SERVER_RECORD_RESPONSE_HEADER";
        constexpr const char* HTTP_CLIENT_RECORD_REQUEST_HEADER = "HTTP_CLIENT_RECORD_REQUEST_HEADER";
        constexpr const char* HTTP_CLIENT_RECORD_REQUEST_COOKIE = "HTTP_CLIENT_RECORD_REQUEST_COOKIE";
        constexpr const char* HTTP_CLIENT_RECORD_RESPONSE_HEADER = "HTTP_CLIENT_RECORD_RESPONSE_HEADER";
        constexpr const char* SQL_MAX_BIND_ARGS_SIZE = "SQL_MAX_BIND_ARGS_SIZE";
        constexpr const char* SQL_ENABLE_SQL_STATS = "SQL_ENABLE_SQL_STATS";
        constexpr const char* SQL_ENABLE_RAW_SQL_CACHE = "SQL_ENABLE_RAW_SQL_CACHE";
        constexpr const char* SQL_TRACE_BIND_VALUE = "SQL_TRACE_BIND_VALUE";
        constexpr const char* CONFIG_FILE = "CONFIG_FILE";
        constexpr const char* ENABLE_CALLSTACK_TRACE = "ENABLE_CALLSTACK_TRACE";
        constexpr const char* ENABLE_CONFIG_FILE_WATCHER = "ENABLE_CONFIG_FILE_WATCHER";
    }

    /**
     * @brief Aggregated runtime configuration used by the Pinpoint agent.
     *
     * The structure mirrors values available in the YAML configuration file and environment
     * overrides. Nested structures keep related options together for readability.
     */
    struct Config {
        // Config generation counter, not a configuration input: make_config()
        // stamps 1 on the first load and old->revision + 1 on every reload,
        // so each published generation is distinguishable by value. Spans and
        // SpanConfigSnapshot carry it out to binding layers, which cache the
        // resolved snapshot and re-fetch only when the revision moved.
        // Directly-constructed Configs (tests) keep 0 ("no resolved config").
        int64_t revision = 0;

        std::string app_name_;
        // Not a configuration input: the agent id is always auto-generated
        // (base64 of a UUIDv7) by make_config()'s identity resolution, so it
        // is process-unique by construction. A reload keeps the running id.
        std::string agent_id_;
        std::string agent_name_;

        // Agent self-identity (ObjectName) version handling. uid_version_ holds the
        // raw configured value of "pinpoint.modules.uid.version" (v1/v3/v4, empty ->
        // v3). object_name_version_ is the resolved ObjectName version (1 for v1/v3,
        // 4 for v4) and drives the gRPC protocol.version header. service_name_ and
        // api_key_ are only used by v4.
        std::string uid_version_;
        std::string service_name_;
        std::string api_key_;
        int object_name_version_ = 1;
        // Set by make_config() from resolve_object_name(): false when a required
        // identity value is missing/invalid for the configured uid version. Gated
        // by check() so startup degrades to a noop agent (Java aborts here).
        // Defaults to true so directly-constructed Config values stay valid.
        bool identity_resolved_ = true;

        // gRPC protocol.version header wire value (Java ProtocolVersion: V1=100, V4=400).
        int protocol_version() const { return object_name_version_ == 4 ? 400 : 100; }
        bool is_v4() const { return object_name_version_ == 4; }

        bool enable = true;
        bool is_container = false;
        bool enable_callstack_trace = false;
        // Opt-in for the config-file watcher (hot reload). Consumed once by
        // Start() to decide whether the watcher is installed, so it is
        // non-reloadable: with the watcher off nothing observes the file,
        // and a running watcher cannot stop itself from its own reload
        // callback.
        bool enable_config_file_watcher = false;

        struct {
            std::string level = defaults::LOG_LEVEL;
            std::string file_path;
            int max_file_size = defaults::LOG_MAX_FILE_SIZE_MB;
        } log;

        struct GrpcSslOptions {
            bool enable = false;
            std::string trust_cert_file_path;
            std::string root_cert_file_path;
        };

        struct GrpcChannelOptions {
            int keepalive_time_ms = defaults::GRPC_KEEPALIVE_TIME_MS;
            int keepalive_timeout_ms = defaults::GRPC_KEEPALIVE_TIMEOUT_MS;
            bool keepalive_permit_without_calls = false;
            int max_send_message_size = defaults::GRPC_MAX_MESSAGE_SIZE;
            int max_receive_message_size = defaults::GRPC_MAX_MESSAGE_SIZE;
            int sender_queue_size = defaults::GRPC_SENDER_QUEUE_SIZE;
        };

        struct {
            std::string host;
            int agent_port = defaults::AGENT_PORT;
            int span_port = defaults::SPAN_PORT;
            int stat_port = defaults::STAT_PORT;

            struct {
                GrpcSslOptions ssl;
                GrpcChannelOptions channel;
            } grpc;

            struct {
                int refresh_interval_ms = defaults::AGENT_INFO_REFRESH_INTERVAL_MS;
                int send_retry_interval_ms = defaults::AGENT_INFO_SEND_RETRY_INTERVAL_MS;
                int max_try_per_attempt = defaults::AGENT_INFO_MAX_TRY_PER_ATTEMPT;
            } agent_info;

            struct {
                int size = defaults::SPAN_BATCH_SIZE;
                int flush_interval_ms = defaults::SPAN_BATCH_FLUSH_INTERVAL_MS;
                int collect_deadline_ms = defaults::SPAN_BATCH_COLLECT_DEADLINE_MS;
                int max_concurrent_requests = defaults::SPAN_BATCH_MAX_CONCURRENT_REQUESTS;
            } span_batch;
        } collector;

        struct {
            bool enable = true;
            int batch_count = defaults::STAT_BATCH_COUNT;
            int collect_interval = defaults::STAT_INTERVAL_MS;
        } stat;

        struct {
            std::string type{COUNTER_SAMPLING};
            int counter_rate = defaults::SAMPLING_COUNTER_RATE;
            double percent_rate = defaults::SAMPLING_PERCENT_RATE;
            int new_throughput = 0;
            int cont_throughput = 0;
        } sampling;

        struct {
            size_t queue_size = defaults::SPAN_QUEUE_SIZE;
            int max_event_depth = defaults::SPAN_MAX_EVENT_DEPTH;
            int max_event_sequence = defaults::SPAN_MAX_EVENT_SEQUENCE;
            // int, not size_t: the parsers produce an int, and a negative
            // value assigned to size_t would wrap past the minimum check in
            // Config::check(), silently disabling span-event chunk flushing.
            int event_chunk_size = defaults::SPAN_EVENT_CHUNK_SIZE;
        } span;

        struct {
            struct {
                bool enable = false;
                int limit = defaults::HTTP_URL_STAT_LIMIT;
                // limit bounds distinct URL keys tracked per snapshot;
                // queue_size bounds per-request records buffered between
                // request end and worker aggregation.
                size_t queue_size = defaults::HTTP_URL_STAT_QUEUE_SIZE;
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
            bool enable_raw_sql_cache = true;
            bool trace_bind_value = true;
        } sql;

        /**
         * @brief Validates required config fields and constraints.
         *
         * @return true when the configuration is valid.
         */
        bool check() const;

        /// @brief Whether a reload from @p old is allowed.
        bool isReloadable(const std::shared_ptr<const Config>& old) const;

        /**
         * @brief Copies the non-reloadable fields (identity, the whole
         * collector section, stat, http url_stat, span queue size and the
         * config-file watcher toggle) from @p old into this one.
         *
         * These cannot change while the agent runs, but a config built for a
         * reload may carry different values, so they are overwritten with the
         * running ones — the reload never disturbs the live agent — and an
         * attempted change is logged by name. No-op when @p old is null.
         */
        void retainNonReloadableFrom(const std::shared_ptr<const Config>& old);
    };

    /**
     * @brief Overrides the config-file watcher poll interval. Test helper.
     *
     * Applies to watchers started by subsequent `ConfigFileWatcher::start()`
     * calls; a running watcher keeps the interval it started with. A
     * non-positive value resets to the production default (1s).
     */
    void set_config_watcher_poll_interval(std::chrono::milliseconds interval);

    /**
     * @brief Polls one configuration file for modification-time changes and
     *        invokes a reload callback when it changed.
     *
     * Owned by the agent that watches its own config file (one watcher per
     * agent, running in the agent's process). stop() joins the watcher
     * thread; a handle inherited across fork() is abandoned instead of
     * joined, so teardown in a forked child never touches a dead thread.
     */
    class ConfigFileWatcher {
    public:
        struct StopSignal;

        /**
         * @param file_path Path of the file to watch; empty disables start().
         * @param reload Invoked on the watcher thread each time the file's
         *        modification time changes. The callee owns re-reading the
         *        sources and applying the result.
         */
        ConfigFileWatcher(std::string file_path, std::function<void()> reload);
        /// @brief Stops the watcher (see stop()).
        ~ConfigFileWatcher();

        ConfigFileWatcher(const ConfigFileWatcher&) = delete;
        ConfigFileWatcher& operator=(const ConfigFileWatcher&) = delete;

        /// @brief Starts the watcher thread. No-op when the path is empty or
        ///        does not exist, or when this process's watcher already runs.
        void start();
        /// @brief Non-blocking half of stop(): signals the current watcher
        ///        generation to wind down without joining it. Used by the
        ///        agent's shutdown signal phase so the watcher drains in
        ///        parallel with the gRPC workers before stop() joins it.
        void requestStop();
        /// @brief Signals the watcher and joins its thread. A joinable handle
        ///        inherited across fork() is abandoned instead (never joined
        ///        or detached — see abandon_thread()).
        void stop();

    private:
        std::string file_path_;
        std::function<void()> reload_;
        std::mutex mutex_;
        std::thread thread_;
        std::shared_ptr<StopSignal> stop_;
        pid_t owner_pid_{0};
    };

    /**
     * @brief Resolves the effective configuration file path for @p options:
     *        the `<env_prefix>_CONFIG_FILE` environment variable when set,
     *        otherwise `options.config_file_path`. Empty when neither is set.
     */
    std::string resolve_config_file_path(const AgentOptions& options);

    /**
     * @brief Builds a `Config` object by combining defaults, the configuration
     *        sources named by @p options and environment overrides.
     *
     * The YAML source is the file named by `resolve_config_file_path(options)`
     * (re-read on every call), falling back to `options.config_yaml`. On the
     * first load (@p old is nullptr) environment overrides are applied and the
     * logger configured immediately. Given @p old — the running agent's config
     * — this is a reload: the config is seeded from @p old so keys absent from
     * the file keep their running values (env-sourced included) instead of
     * reverting to defaults, environment variables are not re-read, the
     * non-reloadable fields are retained, and the logger is reconfigured for
     * the settings that actually changed. Either way the result is final —
     * reload callers pass it straight to `reloadConfig()`.
     *
     * @return Resolved configuration, or nullptr when construction failed.
     */
    std::shared_ptr<Config> make_config(const AgentOptions& options,
                                        const std::shared_ptr<const Config>& old = nullptr);

    /**
     * @brief Serializes a `Config` back into its YAML representation.
     *
     * Runtime-generated AgentId state is omitted, and an AgentName that
     * defaulted to AgentId is emitted as empty so a fresh load preserves the
     * fallback semantics.
     */
    std::string to_config_string(const Config& config);

    /// @brief Config entries whose values differ from defaults, as `Key=Value`.
    std::vector<std::string> to_non_default_config_strings(const Config& config);
}  // namespace pinpoint
