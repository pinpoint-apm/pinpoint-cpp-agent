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

#include <atomic>
#include <cstdlib>
#include <fstream>
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
#include <string_view>
#include <unordered_map>
#include <variant>
#include <unistd.h>

#include "absl/strings/match.h"
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

    // Result of looking up a prefix-resolved env var: the fully resolved name
    // (for accurate logging) and the value (null when unset).
    struct ResolvedEnv {
        std::string name;
        const char* value;
        explicit operator bool() const { return value != nullptr; }
    };

    static ResolvedEnv get_env(const std::string& prefix, const char* suffix) {
        // Full env var name: "<prefix>_<suffix>" (e.g. "PINPOINT_CPP_APPLICATION_NAME").
        std::string name = prefix + "_" + suffix;
        const char* value = std::getenv(name.c_str());
        return {std::move(name), value};
    }

    // Stop signal for one watcher generation. Each watcher captures its own
    // signal by shared_ptr, so a stop issued to one generation can never be
    // undone by a later start: with a single shared signal, a start() racing
    // stop() could reset it before the old watcher — possibly still waiting
    // out its poll tick — observed it, leaving that watcher running forever
    // and the stopper blocked in join().
    //
    // A condition variable rather than sleep+atomic so a stop wakes the
    // watcher immediately; an uninterruptible poll-tick sleep would block
    // stop() — and therefore Shutdown() — for up to a full tick.
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
    // Constant-initialized and trivially destructible, so it is safe to touch
    // during process teardown.
    static std::atomic<std::chrono::milliseconds::rep> config_watcher_poll_interval_ms{
        kDefaultConfigWatcherPollInterval.count()};

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
        config_watcher_poll_interval_ms.store(
            (interval.count() > 0 ? interval : kDefaultConfigWatcherPollInterval).count());
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
        const auto tick = std::chrono::milliseconds(config_watcher_poll_interval_ms.load());

        thread_ = std::thread([path = file_path_, reload = reload_, stop, tick]() {
            // Non-throwing overload: the throwing form could escape this thread
            // function (the file may have been removed since the exists() check
            // above), and an exception leaving a std::thread calls
            // std::terminate(). On error last_write_time stays default-
            // constructed, so the first iteration treats the file as changed.
            std::error_code seed_ec;
            auto last_write_time = std::filesystem::last_write_time(path, seed_ec);

            // wait() covers a stop pending before the tick and one arriving
            // mid-tick, so shutdown need not wait out the polling interval.
            // The check below avoids starting reload work after an observed
            // stop; a stop can still race it, which is why stop() joins this
            // thread — the owning agent stops the watcher before tearing
            // anything down, so an in-flight reload always sees a live agent.
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

    // yaml-cpp resolves map keys case-sensitively; users may write them in any
    // case ("collector"/"Collector"/"COLLECTOR"), so try an exact match first
    // and fall back to a case-insensitive scan. Returns an undefined node when
    // `yaml` is not a map or nothing matches — undefined is falsy, so callers'
    // `if (node)` / default handling is unchanged.
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
            if (kv.first.IsScalar() && absl::EqualsIgnoreCase(kv.first.Scalar(), cname)) {
                return kv.second;
            }
        }
        return YAML::Node(YAML::NodeType::Undefined);
    }

    // Resolves a dotted path ("Collector.Grpc.SslEnable") one case-insensitive
    // segment at a time. Undefined (falsy) when any segment is missing.
    static YAML::Node find_path(const YAML::Node& yaml, std::string_view path) {
        const auto dot = path.find('.');
        if (dot == std::string_view::npos) {
            return find_node(yaml, path);
        }
        return find_path(find_node(yaml, path.substr(0, dot)), path.substr(dot + 1));
    }

    // The getter guards the whole lookup, not just the conversion: yaml-cpp
    // throws from the subscript itself (BadSubscript on scalar nodes) and from
    // element-level conversions (TypedBadConversion<Element> inside vector
    // decoding), all of which derive from YAML::Exception. A malformed config
    // must degrade to defaults, never throw into the embedding application.
    template <typename T>
    static T get_yaml(const YAML::Node& yaml, std::string_view cname, T default_value,
                      const char* type_name) {
        try {
            if (auto node = find_path(yaml, cname)) {
                return node.as<T>();
            }
        } catch (const YAML::Exception& e) {
            LOG_WARN("Failed to read '{}' as {}: {}. Using default value",
                     std::string(cname), type_name, e.what());
        }

        return default_value;
    }

    // Typed yaml readers keyed by the member's own type, so the field table
    // below can dispatch through std::visit. Every reader falls back to the
    // current member value, never a hardcoded default: `config` arrives
    // pre-seeded with the defaults (first load) or the running config (reload).
    // size_t members parse through int so a negative value survives to the
    // range checks in make_config() instead of wrapping.
    static void get_into(const YAML::Node& y, std::string_view key, bool& v) { v = get_yaml(y, key, v, "boolean"); }
    static void get_into(const YAML::Node& y, std::string_view key, int& v) { v = get_yaml(y, key, v, "int"); }
    static void get_into(const YAML::Node& y, std::string_view key, double& v) { v = get_yaml(y, key, v, "double"); }
    static void get_into(const YAML::Node& y, std::string_view key, std::string& v) { v = get_yaml(y, key, v, "string"); }
    static void get_into(const YAML::Node& y, std::string_view key, std::vector<std::string>& v) {
        v = get_yaml(y, key, v, "string vector");
    }
    static void get_into(const YAML::Node& y, std::string_view key, size_t& v) {
        v = static_cast<size_t>(get_yaml(y, key, static_cast<int>(v), "int"));
    }

    // ---- Configuration field table ------------------------------------
    //
    // Every configuration field is declared exactly once here; the YAML
    // loader, the environment loader, to_config_string() and the reload
    // policy all iterate this table. `path` is the dotted YAML location and
    // also fixes the serialization order. `alias`/`env_alias` are deprecated
    // fallbacks, read first so the preferred name wins when both are set.

    template <typename T>
    using FieldGetter = const T& (*)(const Config&);
    using FieldRef = std::variant<FieldGetter<bool>, FieldGetter<int>, FieldGetter<double>,
                                  FieldGetter<size_t>, FieldGetter<std::string>,
                                  FieldGetter<std::vector<std::string>>>;

    struct ConfigField {
        std::string_view path;
        FieldRef ref;
        bool reloadable;
        const char* env;
        std::string_view alias{};
        const char* env_alias = nullptr;
    };

    // The table hands out const references so to_config_string() can read
    // through it; the loaders (which own a non-const Config) cast back.
    template <typename T>
    static T& mut(const T& v) { return const_cast<T&>(v); }

#define REF(member) +[](const Config& c) -> const auto& { return c.member; }
    constexpr bool RELOAD = true;   // applied by a config-file reload
    constexpr bool FIXED = false;   // retained from the running config on reload

    static const ConfigField kConfigFields[] = {
        {"ApplicationName", REF(app_name_), FIXED, env::APPLICATION_NAME},
        {"AgentName", REF(agent_name_), FIXED, env::AGENT_NAME},
        {"UidVersion", REF(uid_version_), FIXED, env::UID_VERSION},
        {"ServiceName", REF(service_name_), FIXED, env::SERVICE_NAME},
        {"ApiKey", REF(api_key_), FIXED, env::API_KEY},
        {"Enable", REF(enable), RELOAD, env::ENABLE},
        {"IsContainer", REF(is_container), RELOAD, env::IS_CONTAINER},
        {"Log.Level", REF(log.level), RELOAD, env::LOG_LEVEL, "LogLevel"},
        {"Log.FilePath", REF(log.file_path), RELOAD, env::LOG_FILE_PATH},
        {"Log.MaxFileSize", REF(log.max_file_size), RELOAD, env::LOG_MAX_FILE_SIZE},
        {"Collector.Host", REF(collector.host), FIXED, env::COLLECTOR_HOST, "Collector.GrpcHost", env::GRPC_HOST},
        {"Collector.AgentPort", REF(collector.agent_port), FIXED, env::COLLECTOR_AGENT_PORT, "Collector.GrpcAgentPort", env::GRPC_AGENT_PORT},
        {"Collector.SpanPort", REF(collector.span_port), FIXED, env::COLLECTOR_SPAN_PORT, "Collector.GrpcSpanPort", env::GRPC_SPAN_PORT},
        {"Collector.StatPort", REF(collector.stat_port), FIXED, env::COLLECTOR_STAT_PORT, "Collector.GrpcStatPort", env::GRPC_STAT_PORT},
        {"Collector.Grpc.SslEnable", REF(collector.grpc.ssl.enable), FIXED, env::GRPC_SSL_ENABLE},
        {"Collector.Grpc.TrustCertFilePath", REF(collector.grpc.ssl.trust_cert_file_path), FIXED, env::GRPC_SSL_TRUST_CERT_FILE_PATH},
        {"Collector.Grpc.RootCertFilePath", REF(collector.grpc.ssl.root_cert_file_path), FIXED, env::GRPC_SSL_ROOT_CERT_FILE_PATH},
        {"Collector.Grpc.KeepAliveTimeMs", REF(collector.grpc.channel.keepalive_time_ms), FIXED, env::GRPC_KEEPALIVE_TIME_MS},
        {"Collector.Grpc.KeepAliveTimeoutMs", REF(collector.grpc.channel.keepalive_timeout_ms), FIXED, env::GRPC_KEEPALIVE_TIMEOUT_MS},
        {"Collector.Grpc.KeepAlivePermitWithoutCalls", REF(collector.grpc.channel.keepalive_permit_without_calls), FIXED, env::GRPC_KEEPALIVE_PERMIT_WITHOUT_CALLS},
        {"Collector.Grpc.MaxSendMessageSize", REF(collector.grpc.channel.max_send_message_size), FIXED, env::GRPC_MAX_SEND_MESSAGE_SIZE},
        {"Collector.Grpc.MaxReceiveMessageSize", REF(collector.grpc.channel.max_receive_message_size), FIXED, env::GRPC_MAX_RECEIVE_MESSAGE_SIZE},
        {"Collector.Grpc.SenderQueueSize", REF(collector.grpc.channel.sender_queue_size), FIXED, env::GRPC_SENDER_QUEUE_SIZE},
        {"Collector.AgentInfo.RefreshIntervalMs", REF(collector.agent_info.refresh_interval_ms), FIXED, env::AGENT_INFO_REFRESH_INTERVAL_MS},
        {"Collector.AgentInfo.SendRetryIntervalMs", REF(collector.agent_info.send_retry_interval_ms), FIXED, env::AGENT_INFO_SEND_RETRY_INTERVAL_MS},
        {"Collector.AgentInfo.MaxTryPerAttempt", REF(collector.agent_info.max_try_per_attempt), FIXED, env::AGENT_INFO_MAX_TRY_PER_ATTEMPT},
        {"Collector.SpanBatch.Size", REF(collector.span_batch.size), FIXED, env::SPAN_BATCH_SIZE},
        {"Collector.SpanBatch.FlushIntervalMs", REF(collector.span_batch.flush_interval_ms), FIXED, env::SPAN_BATCH_FLUSH_INTERVAL_MS},
        {"Collector.SpanBatch.CollectDeadlineMs", REF(collector.span_batch.collect_deadline_ms), FIXED, env::SPAN_BATCH_COLLECT_DEADLINE_MS},
        {"Collector.SpanBatch.MaxConcurrentRequests", REF(collector.span_batch.max_concurrent_requests), FIXED, env::SPAN_BATCH_MAX_CONCURRENT_REQUESTS},
        {"Stat.Enable", REF(stat.enable), FIXED, env::STAT_ENABLE},
        {"Stat.BatchCount", REF(stat.batch_count), FIXED, env::STAT_BATCH_COUNT},
        {"Stat.BatchInterval", REF(stat.collect_interval), FIXED, env::STAT_BATCH_INTERVAL},
        {"Sampling.Type", REF(sampling.type), RELOAD, env::SAMPLING_TYPE},
        {"Sampling.CounterRate", REF(sampling.counter_rate), RELOAD, env::SAMPLING_COUNTER_RATE},
        {"Sampling.PercentRate", REF(sampling.percent_rate), RELOAD, env::SAMPLING_PERCENT_RATE},
        {"Sampling.NewThroughput", REF(sampling.new_throughput), RELOAD, env::SAMPLING_NEW_THROUGHPUT},
        {"Sampling.ContinueThroughput", REF(sampling.cont_throughput), RELOAD, env::SAMPLING_CONTINUE_THROUGHPUT},
        {"Span.QueueSize", REF(span.queue_size), FIXED, env::SPAN_QUEUE_SIZE},
        {"Span.MaxEventDepth", REF(span.max_event_depth), RELOAD, env::SPAN_MAX_EVENT_DEPTH},
        {"Span.MaxEventSequence", REF(span.max_event_sequence), RELOAD, env::SPAN_MAX_EVENT_SEQUENCE},
        {"Span.EventChunkSize", REF(span.event_chunk_size), RELOAD, env::SPAN_EVENT_CHUNK_SIZE},
        {"Http.CollectUrlStat", REF(http.url_stat.enable), FIXED, env::HTTP_COLLECT_URL_STAT},
        {"Http.UrlStatLimit", REF(http.url_stat.limit), FIXED, env::HTTP_URL_STAT_LIMIT},
        {"Http.UrlStatQueueSize", REF(http.url_stat.queue_size), FIXED, env::HTTP_URL_STAT_QUEUE_SIZE},
        {"Http.UrlStatEnableTrimPath", REF(http.url_stat.enable_trim_path), FIXED, env::HTTP_URL_STAT_ENABLE_TRIM_PATH},
        {"Http.UrlStatTrimPathDepth", REF(http.url_stat.trim_path_depth), FIXED, env::HTTP_URL_STAT_TRIM_PATH_DEPTH},
        {"Http.UrlStatMethodPrefix", REF(http.url_stat.method_prefix), FIXED, env::HTTP_URL_STAT_METHOD_PREFIX},
        {"Http.Server.StatusCodeErrors", REF(http.server.status_errors), RELOAD, env::HTTP_SERVER_STATUS_CODE_ERRORS},
        {"Http.Server.ExcludeUrl", REF(http.server.exclude_url), RELOAD, env::HTTP_SERVER_EXCLUDE_URL},
        {"Http.Server.ExcludeMethod", REF(http.server.exclude_method), RELOAD, env::HTTP_SERVER_EXCLUDE_METHOD},
        {"Http.Server.RecordRequestHeader", REF(http.server.rec_request_header), RELOAD, env::HTTP_SERVER_RECORD_REQUEST_HEADER},
        {"Http.Server.RecordRequestCookie", REF(http.server.rec_request_cookie), RELOAD, env::HTTP_SERVER_RECORD_REQUEST_COOKIE},
        {"Http.Server.RecordResponseHeader", REF(http.server.rec_response_header), RELOAD, env::HTTP_SERVER_RECORD_RESPONSE_HEADER},
        {"Http.Client.RecordRequestHeader", REF(http.client.rec_request_header), RELOAD, env::HTTP_CLIENT_RECORD_REQUEST_HEADER},
        {"Http.Client.RecordRequestCookie", REF(http.client.rec_request_cookie), RELOAD, env::HTTP_CLIENT_RECORD_REQUEST_COOKIE},
        {"Http.Client.RecordResponseHeader", REF(http.client.rec_response_header), RELOAD, env::HTTP_CLIENT_RECORD_RESPONSE_HEADER},
        {"Sql.MaxBindArgsSize", REF(sql.max_bind_args_size), RELOAD, env::SQL_MAX_BIND_ARGS_SIZE},
        {"Sql.EnableSqlStats", REF(sql.enable_sql_stats), RELOAD, env::SQL_ENABLE_SQL_STATS},
        {"Sql.EnableRawSqlCache", REF(sql.enable_raw_sql_cache), RELOAD, env::SQL_ENABLE_RAW_SQL_CACHE},
        {"Sql.TraceBindValue", REF(sql.trace_bind_value), RELOAD, env::SQL_TRACE_BIND_VALUE},
        {"EnableCallstackTrace", REF(enable_callstack_trace), RELOAD, env::ENABLE_CALLSTACK_TRACE},
        {"EnableConfigFileWatcher", REF(enable_config_file_watcher), FIXED, env::ENABLE_CONFIG_FILE_WATCHER},
    };
#undef REF

    static void load_yaml_config(const YAML::Node& yaml, Config& config, bool& is_container_set) {
        if (yaml.size() < 1) {
            return;
        }
        for (const auto& f : kConfigFields) {
            std::visit([&](auto ref) {
                auto& value = mut(ref(config));
                // Deprecated key first: the preferred name then overrides it.
                if (!f.alias.empty()) {
                    get_into(yaml, f.alias, value);
                }
                get_into(yaml, f.path, value);
            }, f.ref);
        }
        // The IsContainer key being present (even malformed) means the user
        // decided containerness explicitly, so auto-detection is skipped.
        if (find_path(yaml, "IsContainer")) {
            is_container_set = true;
        }
    }

    template <typename T, typename Parse>
    static T safe_env_parse(Parse parse, const char* desc, const char* env_name,
                            const char* env_value, T default_value) {
        if (auto result = parse(env_value)) {
            return result.value();
        }
        LOG_WARN("{} value '{}' for environment variable '{}'. Using default value: {}",
                 desc, env_value, env_name, default_value);
        return default_value;
    }

    // Typed env readers keyed by the member's own type, mirroring get_into().
    // Like the yaml getters, every parse failure falls back to the CURRENT
    // member value — which at this point carries the yaml-loaded (or default)
    // setting — never a hardcoded default. A malformed env var must degrade to
    // "env override ignored", not silently clobber a value the user set in the
    // config file.
    static void env_into(const ResolvedEnv& e, bool& v) {
        v = safe_env_parse(stob_, "Failed to parse boolean", e.name.c_str(), e.value, v);
    }
    static void env_into(const ResolvedEnv& e, int& v) {
        v = safe_env_parse(stoi_, "Invalid integer", e.name.c_str(), e.value, v);
    }
    static void env_into(const ResolvedEnv& e, double& v) {
        v = safe_env_parse(stod_, "Invalid double", e.name.c_str(), e.value, v);
    }
    static void env_into(const ResolvedEnv& e, std::string& v) { v = e.value; }
    static void env_into(const ResolvedEnv& e, size_t& v) {
        v = static_cast<size_t>(
            safe_env_parse(stoi_, "Invalid integer", e.name.c_str(), e.value, static_cast<int>(v)));
    }
    static void env_into(const ResolvedEnv& e, std::vector<std::string>& v) {
        // SkipEmpty keeps an empty (or all-commas) variable from producing
        // phantom "" entries: e.g. HTTP_SERVER_EXCLUDE_URL="" must clear the
        // list, not build a URL filter around a single empty pattern.
        v = absl::StrSplit(e.value, ',', absl::SkipEmpty());
    }

    static void load_env_config(const std::string& prefix, Config& config, bool& is_container_set) {
        for (const auto& f : kConfigFields) {
            // Deprecated variable first: the preferred name then overrides it.
            for (const char* suffix : {f.env_alias, f.env}) {
                if (!suffix) {
                    continue;
                }
                if (auto e = get_env(prefix, suffix)) {
                    std::visit([&](auto ref) { env_into(e, mut(ref(config))); }, f.ref);
                    if (f.path == "IsContainer") {
                        is_container_set = true;
                    }
                }
            }
        }
    }

    static bool is_container_env() {
        std::error_code ec;
        const char* k = std::getenv("KUBERNETES_SERVICE_HOST");
        return std::filesystem::exists("/.dockerenv", ec) || (k && *k);
    }

    std::string resolve_config_file_path(const AgentOptions& options) {
        if (auto e = get_env(effective_env_prefix(options), env::CONFIG_FILE)) {
            return e.value;
        }
        return options.config_file_path;
    }

    constexpr int MIN_PORT = 1;
    constexpr int MAX_PORT = 65535;
    constexpr double MIN_SAMPLING_PERCENT_RATE = 0.01;
    constexpr double MAX_SAMPLING_PERCENT_RATE = 100.0;
    constexpr int MIN_SPAN_QUEUE_SIZE = 1;
    constexpr int MAX_SPAN_QUEUE_SIZE = 65536;
    constexpr int UNLIMITED_SIZE = -1;
    constexpr int MIN_SPAN_EVENT_DEPTH = 2;
    constexpr int MIN_SPAN_EVENT_SEQUENCE = 4;
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

    // Range/validity checks shared by every clamp site in make_config().
    // in_range: outside [lo, hi] warns and falls back. at_least: below min
    // warns and falls back. clamp_min: below min warns and clamps to min.
    template <typename T>
    static void in_range(T& v, T lo, T hi, T fallback, const char* what) {
        if (v < lo || v > hi) {
            LOG_WARN("{} {} is out of range ({}-{}), using default: {}", what, v, lo, hi, fallback);
            v = fallback;
        }
    }

    template <typename T>
    static void at_least(T& v, T min, T fallback, const char* what) {
        if (v < min) {
            LOG_WARN("{} {} is invalid, using default: {}", what, v, fallback);
            v = fallback;
        }
    }

    template <typename T>
    static void clamp_min(T& v, T min, const char* what) {
        if (v < min) {
            LOG_WARN("{} {} is below minimum, clamping to: {}", what, v, min);
            v = min;
        }
    }

    // Expands %pid% in the log file path at sink-application time (Config keeps
    // the raw value, so reload comparisons and to_config_string() round-trips
    // stay stable). Lets sibling pre-fork workers write separate log files —
    // the built-in size rotation is not multi-process safe on a shared file.
    static std::string expand_log_file_path(const std::string& path) {
        const std::string pid = std::to_string(static_cast<long>(getpid()));
        return absl::StrReplaceAll(path, {{"%pid%", pid}});
    }

    static void apply_log_config(const Config& cfg, const Config* old) {
        // Skip the file logger when its settings are unchanged: setFileLogger()
        // closes and reopens the stream, and a reload triggered by an unrelated
        // setting should not churn the log file. An empty path is applied too —
        // that is how removing FilePath at runtime switches back to stdout.
        // Comparing raw (unexpanded) paths is equivalent: the expansion is
        // constant within one process.
        if (!old || old->log.file_path != cfg.log.file_path ||
            old->log.max_file_size != cfg.log.max_file_size) {
            Logger::getInstance().setFileLogger(
                expand_log_file_path(cfg.log.file_path), cfg.log.max_file_size);
        }
        if (!old || old->log.level != cfg.log.level) {
            Logger::getInstance().setLogLevel(cfg.log.level);
        }
    }

    // Reached from the public StartAgent() entry point, so a parsing problem
    // must never escape into the host: yaml errors degrade to defaults and the
    // function-level handler below is the last-resort backstop. See the
    // contract in config.h for `old` and the finality of the result.
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
        // Environment variables are process-level bootstrap inputs that seed
        // only the first configuration. On a reload (`old` set) they must not
        // be re-applied, or they would silently override values the user just
        // changed in the file. Env-sourced values still survive through the
        // old-config seeding above, unless the file overrides them.
        if (!old) {
            load_env_config(prefix, *config, is_container_set);
        }

        // Configure the logger immediately on the first load so the rest of
        // make_config() (identity resolution, range checks) already logs to
        // the configured sink and level. On a reload the logger is applied at
        // the end instead, once this config is complete and can no longer be
        // rejected.
        if (!old) {
            apply_log_config(*config, nullptr);
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
            // The agent id is not a configuration input: it is empty on the
            // first load (the resolver auto-generates one) and carries the
            // running agent's id on a reload so identity resolution keeps it
            // stable instead of minting a new id.
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

        in_range(config->collector.agent_port, MIN_PORT, MAX_PORT, defaults::AGENT_PORT, "collector agent port");
        in_range(config->collector.span_port, MIN_PORT, MAX_PORT, defaults::SPAN_PORT, "collector span port");
        in_range(config->collector.stat_port, MIN_PORT, MAX_PORT, defaults::STAT_PORT, "collector stat port");

        in_range(config->stat.batch_count, MIN_STAT_BATCH_COUNT, MAX_STAT_BATCH_COUNT,
                 defaults::STAT_BATCH_COUNT, "stat batch count");
        in_range(config->stat.collect_interval, MIN_STAT_INTERVAL_MS, MAX_STAT_INTERVAL_MS,
                 defaults::STAT_INTERVAL_MS, "stat collect interval");

        at_least(config->sampling.counter_rate, 0, 0, "sampling counter rate");
        if (config->sampling.percent_rate < 0.0) {
            LOG_WARN("sampling percent rate {} is invalid, using default: {}",
                     config->sampling.percent_rate, 0.0);
            config->sampling.percent_rate = 0.0;
        } else if (config->sampling.percent_rate < MIN_SAMPLING_PERCENT_RATE) {
            LOG_WARN("sampling percent rate {} is below minimum, clamping to: {}",
                     config->sampling.percent_rate, MIN_SAMPLING_PERCENT_RATE);
            config->sampling.percent_rate = MIN_SAMPLING_PERCENT_RATE;
        } else if (config->sampling.percent_rate > MAX_SAMPLING_PERCENT_RATE) {
            LOG_WARN("sampling percent rate {} exceeds maximum, clamping to: {}",
                     config->sampling.percent_rate, MAX_SAMPLING_PERCENT_RATE);
            config->sampling.percent_rate = MAX_SAMPLING_PERCENT_RATE;
        }
        at_least(config->sampling.new_throughput, 0, 0, "sampling new throughput");
        at_least(config->sampling.cont_throughput, 0, 0, "sampling continue throughput");

        in_range<size_t>(config->span.queue_size, MIN_SPAN_QUEUE_SIZE, MAX_SPAN_QUEUE_SIZE,
                         defaults::SPAN_QUEUE_SIZE, "span queue size");
        if (config->span.max_event_depth == UNLIMITED_SIZE) {
            config->span.max_event_depth = MAX_SPAN_EVENT_DEPTH;
        } else {
            clamp_min(config->span.max_event_depth, MIN_SPAN_EVENT_DEPTH, "span max event depth");
        }
        if (config->span.max_event_sequence == UNLIMITED_SIZE) {
            config->span.max_event_sequence = MAX_SPAN_EVENT_SEQUENCE;
        } else {
            clamp_min(config->span.max_event_sequence, MIN_SPAN_EVENT_SEQUENCE, "span max event sequence");
        }
        at_least(config->span.event_chunk_size, 1, defaults::SPAN_EVENT_CHUNK_SIZE, "span event chunk size");

        at_least(config->collector.span_batch.size, 1, defaults::SPAN_BATCH_SIZE, "span batch size");
        at_least(config->collector.span_batch.flush_interval_ms, 1,
                 defaults::SPAN_BATCH_FLUSH_INTERVAL_MS, "span batch flush interval");
        at_least(config->collector.span_batch.collect_deadline_ms, 0,
                 defaults::SPAN_BATCH_COLLECT_DEADLINE_MS, "span batch collect deadline");
        at_least(config->collector.span_batch.max_concurrent_requests, 1,
                 defaults::SPAN_BATCH_MAX_CONCURRENT_REQUESTS, "span batch max concurrent requests");
        at_least(config->collector.agent_info.refresh_interval_ms, 1,
                 defaults::AGENT_INFO_REFRESH_INTERVAL_MS, "agent info refresh interval");
        at_least(config->collector.agent_info.send_retry_interval_ms, 1,
                 defaults::AGENT_INFO_SEND_RETRY_INTERVAL_MS, "agent info send retry interval");
        at_least(config->collector.agent_info.max_try_per_attempt, 1,
                 defaults::AGENT_INFO_MAX_TRY_PER_ATTEMPT, "agent info max try per attempt");

        at_least(config->sql.max_bind_args_size, 0, 0, "sql max bind args size");

        // A negative limit would cast to a huge size_t at the use site
        // (UrlStatSnapshot::add), disabling the cap and letting the URL map grow
        // unbounded with cardinality. Reject it. Likewise a negative queue size
        // would wrap into a huge size_t, so the range check rejects it too.
        at_least(config->http.url_stat.limit, 0, defaults::HTTP_URL_STAT_LIMIT, "http url stat limit");
        in_range<size_t>(config->http.url_stat.queue_size, MIN_URL_STAT_QUEUE_SIZE, MAX_URL_STAT_QUEUE_SIZE,
                         defaults::HTTP_URL_STAT_QUEUE_SIZE, "http url stat queue size");

        auto& channel = config->collector.grpc.channel;
        at_least(channel.keepalive_time_ms, 0, defaults::GRPC_KEEPALIVE_TIME_MS, "grpc keepalive time");
        at_least(channel.keepalive_timeout_ms, 0, defaults::GRPC_KEEPALIVE_TIMEOUT_MS, "grpc keepalive timeout");
        at_least(channel.max_send_message_size, UNLIMITED_SIZE,
                 defaults::GRPC_MAX_MESSAGE_SIZE, "grpc max send message size");
        at_least(channel.max_receive_message_size, UNLIMITED_SIZE,
                 defaults::GRPC_MAX_MESSAGE_SIZE, "grpc max receive message size");
        in_range(channel.sender_queue_size, MIN_GRPC_QUEUE_SIZE, MAX_GRPC_QUEUE_SIZE,
                 defaults::GRPC_SENDER_QUEUE_SIZE, "grpc sender queue size");

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
            apply_log_config(*config, old.get());
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
        // Flattens the nested map emitted by to_config_string() into
        // ("Dotted.Key", "<emitted value>") pairs in document order.
        void flatten_config(const YAML::Node& node, const std::string& prefix,
                            std::vector<std::pair<std::string, std::string>>& out) {
            for (const auto& kv : node) {
                auto key = kv.first.as<std::string>();
                if (!prefix.empty()) {
                    key = absl::StrCat(prefix, ".", key);
                }
                if (kv.second.IsMap()) {
                    flatten_config(kv.second, key, out);
                } else {
                    YAML::Emitter emitter;
                    emitter << YAML::Flow << kv.second;
                    out.emplace_back(std::move(key), emitter.c_str());
                }
            }
        }
    }

    std::vector<std::string> to_non_default_config_strings(const Config& config) {
        // Diffing against a default-constructed Config keeps this list in
        // lockstep with to_config_string(): a field added there is reported
        // here automatically. Identity and endpoint fields are excluded —
        // they are always explicitly set, so reporting them as "non-default"
        // would be noise.
        static constexpr std::string_view always_set[] = {
            "ApplicationName", "AgentName", "ServiceName", "ApiKey",
            "Enable", "IsContainer", "Collector.Host",
            "Collector.AgentPort", "Collector.SpanPort", "Collector.StatPort",
        };

        std::vector<std::pair<std::string, std::string>> current, defaults;
        flatten_config(YAML::Load(to_config_string(config)), "", current);
        flatten_config(YAML::Load(to_config_string(Config{})), "", defaults);
        const std::unordered_map<std::string, std::string> default_values(defaults.begin(), defaults.end());

        std::vector<std::string> config_strings;
        for (const auto& [key, value] : current) {
            if (std::find(std::begin(always_set), std::end(always_set), key) != std::end(always_set)) {
                continue;
            }
            const auto it = default_values.find(key);
            if (it == default_values.end() || it->second != value) {
                config_strings.push_back(absl::StrCat(key, "=", value));
            }
        }
        return config_strings;
    }

    std::string to_config_string(const Config& config) {
        YAML::Emitter emitter;
        emitter << YAML::BeginMap;

        // `open` is the stack of currently open nested maps; consecutive table
        // entries sharing a path prefix stay inside the same map, so the table
        // order defines the document structure.
        std::vector<std::string_view> open;
        for (const auto& f : kConfigFields) {
            // ServiceName/ApiKey are v4-only inputs.
            if ((f.path == "ServiceName" || f.path == "ApiKey") && !config.is_v4()) {
                continue;
            }

            const std::vector<std::string_view> segs = absl::StrSplit(f.path, '.');
            const size_t depth = segs.size() - 1;
            size_t common = 0;
            while (common < open.size() && common < depth && open[common] == segs[common]) {
                ++common;
            }
            while (open.size() > common) {
                emitter << YAML::EndMap;
                open.pop_back();
            }
            while (open.size() < depth) {
                emitter << YAML::Key << std::string(segs[open.size()]) << YAML::BeginMap;
                open.push_back(segs[open.size()]);
            }

            emitter << YAML::Key << std::string(segs.back()) << YAML::Value;
            if (f.path == "AgentName" && config.agent_name_ == config.agent_id_) {
                // AgentId is runtime-generated state, not a configuration
                // input, and is never serialized. A defaulted AgentName
                // (= AgentId) serializes as empty so loading this YAML in a
                // new process falls back to that process's new id instead of
                // pinning the previous process's id as a display name.
                emitter << std::string();
            } else if (f.path == "ApiKey") {
                // ApiKey is intentionally masked and never serialized in plaintext.
                emitter << (config.api_key_.empty() ? "" : "****");
            } else {
                // yaml-cpp's stlemitter.h emits std::vector as
                // BeginSeq/elements/EndSeq, identical to an explicit loop.
                std::visit([&](auto ref) { emitter << ref(config); }, f.ref);
            }
        }
        while (!open.empty()) {
            emitter << YAML::EndMap;
            open.pop_back();
        }
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

    // The reload policy is the table's `reloadable` column. The runtime
    // identity fields (agent_id_, object_name_version_, identity_resolved_)
    // are not configuration inputs, so they sit outside the table but are
    // pinned alongside the FIXED fields here.
    bool Config::isReloadable(const std::shared_ptr<const Config>& old) const {
        if (!old) return true;
        for (const auto& f : kConfigFields) {
            if (f.reloadable) {
                continue;
            }
            if (!std::visit([&](auto ref) { return ref(*this) == ref(*old); }, f.ref)) {
                return false;
            }
        }
        return agent_id_ == old->agent_id_ && object_name_version_ == old->object_name_version_;
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

        for (const auto& f : kConfigFields) {
            if (!f.reloadable) {
                std::visit([&](auto ref) { mut(ref(*this)) = ref(*old); }, f.ref);
            }
        }
        agent_id_ = old->agent_id_;
        object_name_version_ = old->object_name_version_;
        // Identity was resolved for the running agent; keep that outcome so a
        // reload whose new identity failed resolution still passes check().
        identity_resolved_ = old->identity_resolved_;
    }
}
