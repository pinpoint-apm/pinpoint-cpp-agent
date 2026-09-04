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

#ifndef PINPOINT_TRACER_H
#define PINPOINT_TRACER_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace pinpoint {

    /// @brief Fixed-size binary UID for a normalized SQL statement
    /// (128-bit MurmurHash3 output). Trivially copyable — no heap allocation.
    using SqlUid = std::array<unsigned char, 16>;

    /// @brief Scalar value accepted as a SQL bind argument.
    ///
    /// String views are consumed during SetSqlQuery and need only remain valid
    /// for the duration of that call. nullptr is recorded as "null" and bool
    /// values are recorded as "true" or "false".
    using SqlBindValue = std::variant<
        std::nullptr_t,
        std::string_view,
        bool,
        int32_t,
        uint32_t,
        int64_t,
        uint64_t,
        float,
        double>;

    /// @brief HTTP header names used to propagate Pinpoint trace context.
    inline constexpr std::string_view HEADER_TRACE_ID = "Pinpoint-TraceID";
    inline constexpr std::string_view HEADER_SPAN_ID = "Pinpoint-SpanID";
    inline constexpr std::string_view HEADER_PARENT_SPAN_ID = "Pinpoint-pSpanID";
    inline constexpr std::string_view HEADER_SAMPLED = "Pinpoint-Sampled";
    inline constexpr std::string_view HEADER_FLAG = "Pinpoint-Flags";
    inline constexpr std::string_view HEADER_PARENT_APP_NAME = "Pinpoint-pAppName";
    inline constexpr std::string_view HEADER_PARENT_APP_TYPE = "Pinpoint-pAppType";
    inline constexpr std::string_view HEADER_PARENT_APP_NAMESPACE = "Pinpoint-pAppNamespace";
    inline constexpr std::string_view HEADER_PARENT_SERVICE_NAME = "Pinpoint-pServiceName";
    inline constexpr std::string_view HEADER_HOST = "Pinpoint-Host";

    constexpr int32_t ANNOTATION_API = 12;
    constexpr int32_t ANNOTATION_SQL_ID = 20;
    constexpr int32_t ANNOTATION_SQL_UID = 25;
    constexpr int32_t ANNOTATION_EXCEPTION_ID = -52;
    constexpr int32_t ANNOTATION_HTTP_URL = 40;
    constexpr int32_t ANNOTATION_HTTP_STATUS_CODE = 46;
    constexpr int32_t ANNOTATION_HTTP_COOKIE = 45;
    constexpr int32_t ANNOTATION_HTTP_REQUEST_HEADER = 47;
    constexpr int32_t ANNOTATION_HTTP_RESPONSE_HEADER = 55;
    constexpr int32_t ANNOTATION_HTTP_PROXY_HEADER = 300;

    constexpr int32_t APP_TYPE_CPP = 1300;
    constexpr int32_t SERVICE_TYPE_CPP = APP_TYPE_CPP;
    constexpr int32_t SERVICE_TYPE_CPP_FUNC = 1301;
    constexpr int32_t SERVICE_TYPE_CPP_HTTP_CLIENT = 9800;
    constexpr int32_t SERVICE_TYPE_ASYNC = 100;

    constexpr int32_t SERVICE_TYPE_MYSQL_QUERY = 2101;
    constexpr int32_t SERVICE_TYPE_MSSQL_QUERY = 2201;
    constexpr int32_t SERVICE_TYPE_ORACLE_QUERY = 2301;
    constexpr int32_t SERVICE_TYPE_PGSQL_QUERY = 2501;
    constexpr int32_t SERVICE_TYPE_CASSANDRA_QUERY = 2601;
    constexpr int32_t SERVICE_TYPE_MONGODB_QUERY = 2651;

    constexpr int32_t SERVICE_TYPE_MEMCACHED = 8050;
    constexpr int32_t SERVICE_TYPE_REDIS = 8203;
    constexpr int32_t SERVICE_TYPE_KAFKA = 8660;
    constexpr int32_t SERVICE_TYPE_HBASE = 8800;

    constexpr int32_t SERVICE_TYPE_GRPC_CLIENT = 9160;
    constexpr int32_t SERVICE_TYPE_GRPC_SERVER = 1130;

    constexpr int32_t API_TYPE_DEFAULT = 0;
    constexpr int32_t API_TYPE_WEB_REQUEST = 100;
    constexpr int32_t API_TYPE_INVOCATION = 200;

    constexpr int32_t NONE_ASYNC_ID = 0;

    /// @brief Read-only accessor for inbound propagation carriers.
    class TraceContextReader {
    public:
        virtual ~TraceContextReader() = default;
        /**
         * @brief Reads a key value from the propagation carrier.
         *
         * Returning a view lets implementations avoid a per-lookup value copy;
         * for example, a reader can point into its backing header map.
         *
         * @param key Header name or key to look up. Built-in HTTP headers are
         *        queried with mixed-case constants ("Pinpoint-TraceID",
         *        "X-Forwarded-For", ...); configured recorded-header names are
         *        passed through as configured. HTTP field names are
         *        case-insensitive and HTTP/2/3 deliver them lowercase, so an
         *        HTTP-backed implementation MUST match case-insensitively.
         *        Only non-HTTP carriers with genuinely case-sensitive keys
         *        should use exact matching.
         * @return View of the value if present. The view must remain valid
         *         until the next call on the same reader or until the reader
         *         is destroyed, whichever comes first. Implementations that
         *         build the value on the fly must keep the backing storage
         *         alive accordingly (e.g. in a member buffer).
         */
        virtual std::optional<std::string_view> Get(std::string_view key) const = 0;
    };

    /// @brief Write-only accessor for outbound propagation carriers.
    class TraceContextWriter {
    public:
        virtual ~TraceContextWriter() = default;
        /**
         * @brief Writes a key/value pair into the propagation carrier.
         *
         * @warning @p key and @p value are borrowed views that remain valid only
         *          for the duration of this call. Implementations must consume or
         *          copy their contents before returning and must not retain the
         *          views or pointers into their backing storage.
         *
         * @param key Case-insensitive header name or key.
         * @param value Value to set.
         */
        virtual void Set(std::string_view key, std::string_view value) = 0;
    };

    /// @brief Enumerates logical header groups that can be recorded on spans.
    enum HeaderType {
        HTTP_REQUEST = 0, HTTP_RESPONSE, HTTP_COOKIE
    };

    /// @brief Interface used to iterate through headers without exposing container details.
    class HeaderReader : public TraceContextReader {
    public:
        virtual ~HeaderReader() override = default;
        /// @brief See TraceContextReader::Get for the returned view's lifetime.
        virtual std::optional<std::string_view> Get(std::string_view key) const override = 0;
        /// @brief Iterates all headers; return false from @p callback to stop early.
        virtual void ForEach(std::function<bool(std::string_view key, std::string_view val)> callback) const = 0;
    };

    /// @brief Carrier that both reads and writes, for an in-place header map.
    ///        Each method keeps the contract of the base it overrides.
    class HeaderReaderWriter : public HeaderReader, public TraceContextWriter {
    public:
        virtual ~HeaderReaderWriter() override = default;
        virtual std::optional<std::string_view> Get(std::string_view key) const override = 0;
        virtual void ForEach(std::function<bool(std::string_view key, std::string_view val)> callback) const override = 0;
        virtual void Set(std::string_view key, std::string_view value) override = 0;
    };

    /// @brief Interface used to enumerates frames stored inside a call stack.
    class CallStackReader {
    public:
        virtual ~CallStackReader() = default;
        /// @brief Invokes @p callback with module, function, file and line for
        ///        each frame in the call stack.
        virtual void ForEach(std::function<void(std::string_view module, std::string_view function, std::string_view file, int line)> callback) const = 0;
    };

    /// @brief One pre-collected call stack frame for the frame-list SetError
    ///        overload. The views are consumed during that call and copied
    ///        only when the event is recording.
    struct CallStackFrame {
        std::string_view module;
        std::string_view function;
        std::string_view file;
        int line;
    };
    
    class Span;
    using SpanPtr = std::shared_ptr<Span>;

    /// @brief Interface describing a span event recorded within a span.
    class SpanEvent {
    public:
        virtual ~SpanEvent() = default;

        /// @brief Sets the service type for the span event.
        virtual void SetServiceType(int32_t type) = 0;
        /// @brief Sets the logical operation recorded by the event.
        virtual void SetOperationName(std::string_view operation) = 0;
        /// @brief Records the event's start timestamp.
        virtual void SetStartTime(std::chrono::system_clock::time_point start_time) = 0;
        /// @brief Records the destination identifier (for RPCs).
        virtual void SetDestination(std::string_view dest) = 0;
        /// @brief Records the remote endpoint.
        virtual void SetEndPoint(std::string_view end_point) = 0;
        /// @brief Stores an error message.
        virtual void SetError(std::string_view error_message) = 0;
        /// @brief Stores a named error message.
        virtual void SetError(std::string_view error_name, std::string_view error_message) = 0;
        /// @brief Stores an error message along with call stack details.
        virtual void SetError(std::string_view error_name, std::string_view error_message, CallStackReader& reader) = 0;
        /// @brief Stores an error message along with a pre-collected call
        ///        stack (innermost-last, the order a reader would emit).
        virtual void SetError(std::string_view error_name, std::string_view error_message,
                              const std::vector<CallStackFrame>& frames) = 0;
        /// @brief Records a SQL query and its bound parameters, joined with
        ///        ", " up to the configured bind-value size limit.
        virtual void SetSqlQuery(std::string_view sql_query,
                                 const std::vector<SqlBindValue>& bind_args) = 0;
        /// @brief Records HTTP headers into the event.
        virtual void RecordHeader(HeaderType which, HeaderReader& reader) = 0;
        /// @brief Injects the trace context for the outbound call represented
        ///        by this span event into an outbound carrier.
        virtual void InjectContext(TraceContextWriter& writer) = 0;
        /// @brief Records the child span id propagated downstream as the
        ///        Pinpoint-SpanID header, for callers that build the outbound
        ///        propagation headers themselves instead of using
        ///        InjectContext (which generates and records one internally).
        virtual void SetNextSpanId(int64_t next_span_id) = 0;

        /// @brief Records a 32-bit integer annotation on this span event.
        virtual void SetAnnotation(int32_t key, int32_t value) = 0;
        /// @brief Records a 64-bit integer annotation on this span event.
        virtual void SetAnnotation(int32_t key, int64_t value) = 0;
        /// @brief Records a string annotation. The view is consumed during
        ///        this call and is copied only when the event is recording.
        virtual void SetAnnotation(int32_t key, std::string_view value) = 0;
        /// @brief Records a string-string annotation. Both views are consumed
        ///        during this call and copied only when the event is recording.
        virtual void SetAnnotation(int32_t key,
                                   std::string_view value1,
                                   std::string_view value2) = 0;
        /// @brief Finalizes this span event through its parent span.
        ///        Guarded against duplicate calls: ending an already-ended
        ///        event is a warning no-op, like Span::EndSpan.
        virtual void EndEvent() = 0;
    };

    /// @brief Non-owning span-event pointer.
    ///
    /// Span events are owned by their parent span. Ending an event retains its
    /// storage so a duplicate EndEvent() can be a warning no-op while the parent
    /// span remains alive, but this pointer does not extend that lifetime. Do not
    /// retain it for work that can outlive the span. It also inherits the span's
    /// single-thread contract: concurrent use, including ending it from another
    /// thread, is unsupported and can race with event mutation or span teardown.
    using SpanEventPtr = SpanEvent*;

    /// @brief Final resolved configuration captured by one span at creation.
    ///
    /// Binding layers that manage propagation, header annotations or event
    /// positions outside this library use this snapshot instead of re-parsing
    /// the agent's input configuration. Header arrays are indexed by
    /// HeaderType (HTTP_REQUEST / HTTP_RESPONSE / HTTP_COOKIE).
    struct SpanConfigSnapshot {
        std::string application_name;
        int32_t application_type = APP_TYPE_CPP;
        std::string service_name;
        int32_t max_event_depth = 0;
        int32_t max_event_sequence = 0;
        std::array<std::vector<std::string>, 3> http_server_headers;
        std::array<std::vector<std::string>, 3> http_client_headers;
        /// Resolved Sql.TraceBindValue. A binding layer that formats bind
        /// arguments itself needs this: the agent gates its own bind-value
        /// formatting on the same setting, so without it the binding would have
        /// to guess, and passing values it should not have collected is a
        /// privacy leak rather than a cosmetic mismatch.
        ///
        /// Defaults to false, unlike Config::sql::trace_bind_value, which
        /// defaults to true: a default-constructed snapshot means "no resolved
        /// config" (see revision), and for a capture flag the no-information
        /// answer has to be "do not capture".
        bool sql_trace_bind_value = false;
        /// Config generation this snapshot was built from: 1 for the initial
        /// load, incremented by each config-file hot reload. 0 means the
        /// snapshot carries no resolved config (default-constructed). Binding
        /// layers cache the agent snapshot and compare this against
        /// Span::GetConfigRevision() to refresh only when a reload happened.
        int64_t revision = 0;
    };

    /**
     * @brief Interface implemented by concrete spans managed by the Pinpoint agent.
     *
     * @warning Thread-safety contract: a single `Span` instance is NOT safe for
     *          concurrent use. All methods on one span — including those of the
     *          `SpanEvent`s it hands out — must be called from a single thread
     *          for the lifetime of that span. Sharing one `SpanPtr` across
     *          threads and calling into it concurrently is undefined behaviour
     *          and can crash the process (use-after-free of span events,
     *          heap corruption of the internal string/annotation/exception
     *          buffers), independent of any data-consistency concerns.
     *
     *          To trace work that runs on another thread, do NOT share this span.
     *          Instead call NewAsyncSpan() on the owning thread to obtain a
     *          separate child span, then use that child exclusively on the other
     *          thread. Each span instance thus remains single-threaded.
     */
    class Span {
    public:
        virtual ~Span() = default;

        /// @brief Creates a new span event using the default service type.
        virtual SpanEventPtr NewSpanEvent(std::string_view operation) = 0;
        /// @brief Creates a new span event using the specified service type.
        virtual SpanEventPtr NewSpanEvent(std::string_view operation, int32_t service_type) = 0;
        /// @brief Returns the active (top-of-stack) span event, or a shared
        /// no-op event (never null) when the span is finished or has no
        /// active event.
        virtual SpanEventPtr GetSpanEvent() = 0;
        /// @brief Finalizes the span and submits its recorded data for
        ///        asynchronous delivery. Span events are finalized individually
        ///        via SpanEvent::EndEvent on the event handle.
        virtual void EndSpan() = 0;
        /// @brief Creates an asynchronous child span for background work.
        ///
        /// Use this to continue the trace on another thread without sharing the
        /// parent span. Call it on the thread that owns this span; the returned
        /// child is a distinct instance that the other thread then uses
        /// exclusively. See the Span thread-safety contract above.
        virtual SpanPtr NewAsyncSpan(std::string_view async_operation) = 0;
        /// @brief Creates an asynchronous child span linked by caller-supplied
        ///        async ids, for wrappers that manage span events outside this
        ///        library (see RecordSpanEvent).
        ///
        /// The single-argument overload reads the async link from this span's
        /// active native span event; a wrapper that records its events itself
        /// has none, so it assigns `async_id` to its own parent event (flushed
        /// later with that event) and passes the per-event `async_sequence`
        /// here instead.
        virtual SpanPtr NewAsyncSpan(std::string_view async_operation,
                                     int32_t async_id, int32_t async_sequence) = 0;
        /// @brief Records one already-completed span event (batch replay).
        ///
        /// For wrappers that create, position and time their span events
        /// themselves and flush them in one batch at span end: sequence,
        /// depth and both timestamps (epoch milliseconds) come from the
        /// caller instead of this span's own counters and clock. The event is
        /// already complete on arrival, so it is finalized before this
        /// returns: the returned handle is a finished event — its setters
        /// (annotations included) are warning no-ops and EndEvent() on it is
        /// the duplicate-end no-op, so do not call it. `async_id` marks an
        /// event that spawned async children (NONE_ASYNC_ID otherwise).
        /// Events beyond the configured max depth/sequence are dropped (a
        /// shared no-op event is returned).
        virtual SpanEventPtr RecordSpanEvent(std::string_view operation,
                                             int32_t service_type,
                                             int32_t sequence, int32_t depth,
                                             int64_t start_time_ms,
                                             int64_t end_time_ms,
                                             int32_t async_id) = 0;

        /// @brief Returns the distributed trace identifier for the span in its
        ///        wire form (`agentId^startTime^sequence`), or an empty string
        ///        when the span carries no trace (e.g. a noop/unsampled span).
        virtual std::string GetTraceId() = 0;
        /// @brief Returns the span identifier.
        virtual int64_t GetSpanId() = 0;
        /// @brief Indicates whether the span is sampled.
        virtual bool IsSampled() = 0;
        /// @brief Returns the final config generation captured by this span.
        ///
        /// The default keeps third-party Span implementations source-compatible;
        /// production sampled spans override it with their native snapshot.
        virtual SpanConfigSnapshot GetConfigSnapshot() const { return {}; }
        /// @brief Returns the revision of the config generation captured by
        ///        this span (SpanConfigSnapshot::revision) without building
        ///        the snapshot. Binding layers compare it against their cached
        ///        Agent::GetConfigSnapshot() revision on every span creation
        ///        and re-fetch the snapshot only when a reload bumped it. The
        ///        default (0, "no resolved config") keeps third-party Span
        ///        implementations source-compatible.
        virtual int64_t GetConfigRevision() const { return 0; }

        /// @brief Sets the span service type.
        virtual void SetServiceType(int32_t service_type) = 0;
        /// @brief Records the span start time.
        virtual void SetStartTime(std::chrono::system_clock::time_point start_time) = 0;
        /// @brief Records the remote address.
        virtual void SetRemoteAddress(std::string_view address) = 0;
        /// @brief Records the endpoint served by the span.
        virtual void SetEndPoint(std::string_view end_point) = 0;
        /// @brief Records the host of acceptor.
        virtual void SetAcceptorHost(std::string_view host) = 0;
        /// @brief Records an error message at the span level.
        virtual void SetError(std::string_view error_message) = 0;
        /// @brief Records a named error message at the span level.
        virtual void SetError(std::string_view error_name, std::string_view error_message) = 0;
        /// @brief Records the HTTP status code for the span.
        virtual void SetStatusCode(int status) = 0;
        /// @brief Records URL statistics for the span.
        virtual void SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) = 0;
        /// @brief Records the logging flag and injects the span context into a logger.
        virtual void SetLogging(TraceContextWriter& writer) = 0;
        /// @brief Records HTTP headers for the span.
        virtual void RecordHeader(HeaderType which, HeaderReader& reader) = 0;

        /// @brief Records a 32-bit integer annotation on the span.
        virtual void SetAnnotation(int32_t key, int32_t value) = 0;
        /// @brief Records a 64-bit integer annotation on the span.
        virtual void SetAnnotation(int32_t key, int64_t value) = 0;
        /// @brief Records a string annotation. The view is consumed during
        ///        this call and is copied only when the span is recording.
        virtual void SetAnnotation(int32_t key, std::string_view value) = 0;
        /// @brief Records a string-string annotation. Both views are consumed
        ///        during this call and copied only when the span is recording.
        virtual void SetAnnotation(int32_t key,
                                   std::string_view value1,
                                   std::string_view value2) = 0;
        /// @brief Records a composite long/int/int/byte/byte/string annotation
        ///        (the ANNOTATION_HTTP_PROXY_HEADER payload shape). The view is
        ///        consumed during this call and copied only when the span is
        ///        recording.
        virtual void SetAnnotation(int32_t key, int64_t long_value,
                                   int32_t int_value1, int32_t int_value2,
                                   int32_t byte_value1, int32_t byte_value2,
                                   std::string_view string_value) = 0;
    };

    /// @brief Interface exposed to application code for creating spans.
    class Agent {
    public:
        virtual ~Agent() = default;

        /// @brief Creates a new span for an outbound RPC/operation.
        virtual SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point) = 0;
        /// @brief Creates a new span, extracting context from @p reader.
        virtual SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point, TraceContextReader& reader) = 0;
        /// @brief Creates a new span, also recording the HTTP @p method.
        virtual SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point, std::string_view method, TraceContextReader& reader) = 0;
        /// @brief Creates a new span, extracting context from a pre-extracted
        ///        map of Pinpoint propagation headers keyed by their canonical
        ///        names (e.g. HEADER_TRACE_ID). Callers that already dumped the
        ///        Pinpoint-* headers (binding layers) pass them here instead of
        ///        implementing a TraceContextReader.
        ///        @p method is the HTTP request method, needed for the
        ///        Http.Server.ExcludeMethod filter — that filter is skipped for
        ///        an empty method, which means "not HTTP" (messaging consumers,
        ///        gRPC) exactly as it does on the reader overload above.
        virtual SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point,
                                std::string_view method,
                                const std::map<std::string, std::string>& pinpoint_headers) = 0;
        /// @brief Returns the agent's current resolved config snapshot.
        ///        Binding layers fetch it once after StartAgent() and again
        ///        whenever a created span reports a GetConfigRevision() newer
        ///        than the cached snapshot's revision. The default (an empty
        ///        snapshot with revision 0) keeps third-party Agent
        ///        implementations source-compatible.
        virtual SpanConfigSnapshot GetConfigSnapshot() const { return {}; }
        /// @brief Returns whether agent initialization succeeded and tracing is
        ///        enabled. Individual spans may still be rejected by sampling.
        ///        Always false for an agent handle inherited across fork() —
        ///        each process must obtain its own agent via StartAgent().
        ///        False until registration with the collector succeeds: see
        ///        StartAgent() for what is (not) recorded until then.
        virtual bool Enable() = 0;
        /// @brief Stops the agent and waits for its workers to finish. Pending
        ///        spans are submitted when a channel is available, but delivery
        ///        is not guaranteed.
        ///
        /// Bounded: Shutdown() returns within a 3-second deadline even if a
        /// worker is wedged in an RPC that ignores cancellation. Stragglers
        /// then keep draining on a background thread that holds the agent
        /// alive until they finish, so the final release of the object may
        /// happen after Shutdown() returns. Dropping the last AgentPtr
        /// without calling Shutdown() is bounded the same way — destruction
        /// is deferred until the workers finish (the object stays leaked if
        /// they never do) rather than blocking the release.
        ///
        /// Terminal for this instance: it can never be brought back online,
        /// Enable() stays false and NewSpan() only returns noop spans. When
        /// this is the global agent it is also removed from the singleton, so
        /// GlobalAgent() falls back to the noop agent until a new one is
        /// installed.
        ///
        /// To resume tracing in the same process, call StartAgent() again;
        /// GlobalAgent() then hands out the fresh agent. Note that each such
        /// cycle re-resolves the agent identity, so it registers a NEW agent
        /// instance with the collector.
        virtual void Shutdown() = 0;
    };

    using AgentPtr = std::shared_ptr<Agent>;

    /// @brief Default application type used by StartAgent().
    constexpr int32_t DEFAULT_APP_TYPE = APP_TYPE_CPP;
    /// @brief Default server metadata description used by StartAgent().
    constexpr std::string_view DEFAULT_SERVER_INFO = "C/C++ Application";

    /**
     * @brief Inputs for StartAgent(): configuration sources, server metadata
     *        and per-worker naming.
     *
     * Configuration source precedence: when @ref config_file_path is set —
     * including via the `<env_prefix>_CONFIG_FILE` environment variable, which
     * overrides it — the file's content replaces @ref config_yaml wholesale on
     * every (re)load; the two sources are never merged. Environment variables
     * (`<env_prefix>_*`) still override individual settings from either source.
     */
    struct AgentOptions {
        /// YAML configuration file path. When set, the file's content replaces
        /// config_yaml wholesale. With `EnableConfigFileWatcher: true`
        /// (default: false) the config-file watcher re-reads the file on
        /// change and hot-reloads the reloadable settings.
        std::string config_file_path;
        /// Raw YAML configuration used when no config file is configured.
        std::string config_yaml;
        /// Prefix of the environment variable names the agent reads (e.g.
        /// `MYAPP` makes it read `MYAPP_APPLICATION_NAME`). Empty selects the
        /// default `PINPOINT_CPP`.
        std::string env_prefix;

        /// Application type reported to the collector.
        int32_t app_type = DEFAULT_APP_TYPE;
        /// Server runtime description included in AgentInfo.
        std::string server_info = std::string(DEFAULT_SERVER_INFO);
        /// Process command line arguments included in AgentInfo.
        std::vector<std::string> args;
        /// Loaded service libraries included in AgentInfo.
        std::vector<std::string> libs;
    };

    /**
     * @brief Creates, configures and starts the agent in the CURRENT process,
     *        and installs it as the global agent.
     *
     * This is the only way to bring an agent online. Call it in the process
     * that will record spans — for pre-fork servers (nginx, Apache prefork,
     * uWSGI, ...) that means each worker calls StartAgent() after fork(), from
     * its post-fork/worker-init hook, and the master process makes NO agent
     * API calls at all. Each worker registers as its own agent instance with a
     * process-unique agent id and its own start time.
     *
     * The call returns as soon as initialization is launched: the config-file
     * watcher is installed (only when enabled via `EnableConfigFileWatcher`,
     * which defaults to false) and an initialization thread opens the gRPC
     * channels, registers with the collector and starts the workers. It does
     * NOT wait for collector connection or registration; Enable() flips to
     * true once registration succeeds.
     *
     * @warning Nothing is recorded before that first registration succeeds:
     * NewSpan() returns noop spans and no agent, URL or system statistics are
     * collected, so a collector whose agent port (9991) alone is unreachable
     * yields zero spans even though the span and stat ports are open. The
     * registration is retried indefinitely, and the agent logs an INFO line
     * every 30 seconds while it waits, naming this consequence. This differs
     * from the Java agent, which sends spans while registration retries in
     * the background.
     *
     * @return true when the agent was launched and installed as the global
     * agent — obtain the handle with GlobalAgent(). false on a configuration
     * or setup failure (never an exception); check the agent log for the
     * cause. Nothing is installed as the global agent then — GlobalAgent()
     * keeps returning the noop agent — and a later StartAgent() call retries
     * from scratch.
     *
     * Calling StartAgent() again in the same process leaves the already
     * running agent untouched and returns true (with a warning). After
     * Shutdown() a new StartAgent() call builds a fresh agent.
     *
     * @warning An agent handle (or the global agent) inherited across fork()
     * is unusable in the child and is refused: its threads and gRPC runtime
     * do not exist there, and gRPC cannot be re-initialized in a process that
     * forked after `grpc_init`. An inherited agent reports Enable() == false
     * and hands out noop spans, and a StartAgent() call in such a child is
     * refused (returns false) and evicts the inherited global agent, so
     * GlobalAgent() degrades to the noop agent. The child must be a fresh
     * process that calls StartAgent() itself before any other agent use.
     */
    bool StartAgent(const AgentOptions& options = {});

    /// @brief Returns the singleton global agent instance installed by
    ///        StartAgent(), or the noop agent when none is installed.
    AgentPtr GlobalAgent();

    namespace helper {
        /// @brief Records an inbound request on @p span: the endpoint, the
        ///        remote address (taken from X-Forwarded-For / X-Real-Ip when
        ///        present, else @p remote_addr), the Pinpoint proxy headers,
        ///        and the request headers named in the configuration.
        ///        A null @p span is ignored.
        void TraceHttpServerRequest(SpanPtr span, std::string_view remote_addr, std::string_view endpoint, HeaderReader& request_reader);

        /// @brief As above, additionally recording the configured cookies.
        void TraceHttpServerRequest(SpanPtr span, std::string_view remote_addr, std::string_view endpoint, HeaderReader& request_reader, HeaderReader& cookie_reader);

        /// @brief Records the response on @p span: the status code, URL stats
        ///        for @p url_pattern / @p method, and the configured response
        ///        headers. A null @p span is ignored.
        void TraceHttpServerResponse(SpanPtr span, std::string_view url_pattern, std::string_view method, int32_t status_code, HeaderReader& response_reader);

        /// @brief Records an outbound call on @p span_event: service type
        ///        SERVICE_TYPE_CPP_HTTP_CLIENT, @p host as both endpoint and
        ///        destination, @p url as ANNOTATION_HTTP_URL, and the
        ///        configured request headers. A null @p span_event is ignored.
         void TraceHttpClientRequest(SpanEventPtr span_event, std::string_view host, std::string_view url, HeaderReader& request_reader);

         /// @brief As above, additionally recording the configured cookies.
         void TraceHttpClientRequest(SpanEventPtr span_event, std::string_view host, std::string_view url, HeaderReader& request_reader, HeaderReader& cookie_reader);

        /// @brief Records the client response on @p span_event: the status code
        ///        as ANNOTATION_HTTP_STATUS_CODE and the configured response
        ///        headers. A null @p span_event is ignored.
        void TraceHttpClientResponse(SpanEventPtr span_event, int32_t status_code, HeaderReader& response_reader);

        // RAII helper to manage span events.
        class ScopedSpanEvent {
        public:
            explicit ScopedSpanEvent(const SpanPtr& span, std::string_view operation) : span_(span) {
                if (span_) {
                    event_ = span_->NewSpanEvent(operation, SERVICE_TYPE_CPP_FUNC);
                }
            }
            explicit ScopedSpanEvent(const SpanPtr& span, std::string_view operation, int32_t service_type) : span_(span) {
                if (span_) {
                    event_ = span_->NewSpanEvent(operation, service_type);
                }
            }

            // Non-copyable (and thereby non-movable) scope guard: a copy
            // would call EndEvent() once per instance on the same raw event
            // — a warn no-op while the span lives, a dangling access after
            // it. C++17 guaranteed elision keeps direct initialization from
            // a prvalue (auto guard = ScopedSpanEvent(span, "op")) working.
            ScopedSpanEvent(const ScopedSpanEvent&) = delete;
            ScopedSpanEvent& operator=(const ScopedSpanEvent&) = delete;

            ~ScopedSpanEvent() {
                if (event_) {
                    // A destructor is implicitly noexcept, so anything
                    // escaping EndEvent() would std::terminate the host
                    // process. Current library builds already catch inside
                    // EndEvent(), but this header is compiled into the host
                    // and may run against an older library — swallow
                    // defensively.
                    try {
                        event_->EndEvent();
                    } catch (...) {
                    }
                }
            }
        
            SpanEventPtr operator->() const { return event_; }
            SpanEventPtr value() const { return event_; }
        
        private:
            SpanPtr span_;
            SpanEventPtr event_{nullptr};
        };
   
    };
    
}  // namespace pinpoint

#endif //PINPOINT_TRACER_H
