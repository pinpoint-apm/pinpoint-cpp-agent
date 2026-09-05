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

/**
 * @file tracer_c.h
 * @brief Pure-C public API for the Pinpoint C++ agent.
 *
 * This header is intentionally free of any C++ constructs so that it can be
 * included from both C and C++ translation units.  C++ programs may use either
 * this header or the richer pinpoint/tracer.h interface.
 *
 * ## Quick-start (C)
 * @code
 *   #include "pinpoint/tracer_c.h"
 *
 *   // Configure and start the agent in the process that records spans.
 *   pt_agent_options_t opts = pt_agent_options_new();
 *   pt_agent_options_set_config_file(opts, "/etc/pinpoint/agent.yaml");
 *   if (!pt_start_agent(opts)) {
 *       fprintf(stderr, "failed to start the pinpoint agent: check the agent log\n");
 *   }
 *   pt_agent_options_free(opts);
 *   pt_agent_t agent = pt_global_agent();
 *
 *   // --- incoming request ---
 *   pt_context_reader_t reader = { &my_headers, my_header_get };
 *   pt_span_t span = pt_agent_new_span_with_reader(agent, "MyService", "/api/v1", &reader);
 *
 *   // create a child event
 *   pt_span_event_t se = pt_span_new_event(span, "db_query");
 *   pt_span_event_set_service_type(se, PT_SERVICE_TYPE_MYSQL_QUERY);
 *   pt_span_event_set_annotation_string(se, PT_ANNOTATION_HTTP_URL, "/api/v1");
 *   pt_span_event_end(se);
 *   pt_span_event_destroy(se);
 *
 *   pt_span_end(span);
 *   pt_span_destroy(span);
 *
 *   pt_agent_shutdown(agent);
 *   pt_agent_destroy(agent);
 * @endcode
 *
 * Span-event handles are non-owning views that hold no resources of their
 * own: pt_span_event_destroy() is a safe no-op kept for API symmetry. Ending
 * a span or event seals its recording state but retains these objects while
 * the parent span is alive. Do not retain a view beyond the parent span's
 * lifetime.
 *
 * ## Handle validity
 *
 * Agent and span handles are owning and must be released with their
 * pt_*_destroy(). Passing an unknown or already-destroyed handle to any
 * function in this header is ignored with a warning, or returns that
 * function's documented failure value — it is never undefined behavior.
 * NULL and the shared disabled-agent / no-op-span sentinels are ignored
 * silently.
 */

#ifndef PINPOINT_TRACER_C_H
#define PINPOINT_TRACER_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* Propagation header name constants                                            */
/* ========================================================================== */

#define PT_HEADER_TRACE_ID             "Pinpoint-TraceID"
#define PT_HEADER_SPAN_ID              "Pinpoint-SpanID"
#define PT_HEADER_PARENT_SPAN_ID       "Pinpoint-pSpanID"
#define PT_HEADER_SAMPLED              "Pinpoint-Sampled"
#define PT_HEADER_FLAG                 "Pinpoint-Flags"
#define PT_HEADER_PARENT_APP_NAME      "Pinpoint-pAppName"
#define PT_HEADER_PARENT_APP_TYPE      "Pinpoint-pAppType"
#define PT_HEADER_PARENT_APP_NAMESPACE "Pinpoint-pAppNamespace"
#define PT_HEADER_PARENT_SERVICE_NAME  "Pinpoint-pServiceName"
#define PT_HEADER_HOST                 "Pinpoint-Host"

/* ========================================================================== */
/* Annotation key constants                                                     */
/* ========================================================================== */

#define PT_ANNOTATION_API                   12
#define PT_ANNOTATION_SQL_ID                20
#define PT_ANNOTATION_SQL_UID               25
#define PT_ANNOTATION_EXCEPTION_ID          (-52)
#define PT_ANNOTATION_HTTP_URL              40
#define PT_ANNOTATION_HTTP_STATUS_CODE      46
#define PT_ANNOTATION_HTTP_COOKIE           45
#define PT_ANNOTATION_HTTP_REQUEST_HEADER   47
#define PT_ANNOTATION_HTTP_RESPONSE_HEADER  55
#define PT_ANNOTATION_HTTP_PROXY_HEADER     300

/* ========================================================================== */
/* Service / application type constants                                         */
/* ========================================================================== */

#define PT_APP_TYPE_CPP                 1300
#define PT_SERVICE_TYPE_CPP             PT_APP_TYPE_CPP
#define PT_SERVICE_TYPE_CPP_FUNC        1301
#define PT_SERVICE_TYPE_CPP_HTTP_CLIENT 9800
#define PT_SERVICE_TYPE_ASYNC           100

#define PT_SERVICE_TYPE_MYSQL_QUERY     2101
#define PT_SERVICE_TYPE_MSSQL_QUERY     2201
#define PT_SERVICE_TYPE_ORACLE_QUERY    2301
#define PT_SERVICE_TYPE_PGSQL_QUERY     2501
#define PT_SERVICE_TYPE_CASSANDRA_QUERY 2601
#define PT_SERVICE_TYPE_MONGODB_QUERY   2651

#define PT_SERVICE_TYPE_MEMCACHED       8050
#define PT_SERVICE_TYPE_REDIS           8203
#define PT_SERVICE_TYPE_KAFKA           8660
#define PT_SERVICE_TYPE_HBASE           8800

#define PT_SERVICE_TYPE_GRPC_CLIENT     9160
#define PT_SERVICE_TYPE_GRPC_SERVER     1130

#define PT_API_TYPE_DEFAULT             0
#define PT_API_TYPE_WEB_REQUEST         100
#define PT_API_TYPE_INVOCATION          200

#define PT_NONE_ASYNC_ID                0

/* ========================================================================== */
/* Opaque handle types                                                          */
/* ========================================================================== */

/** Opaque handle to a Pinpoint agent instance. */
typedef struct pt_agent_s*      pt_agent_t;
/** Opaque handle to a distributed trace span. */
typedef struct pt_span_s*       pt_span_t;
/** Non-owning handle to a span event (child operation). */
typedef struct pt_span_event_s* pt_span_event_t;

/* ========================================================================== */
/* Trace identifier                                                             */
/* ========================================================================== */

/**
 * Buffer size that always holds a full trace id (wire form
 * `"<agent_id>^<start_time>^<sequence>"`) including its NUL terminator:
 * agent id (<= 24) + '^' + int64 (<= 20) + '^' + int64 (<= 20), rounded up.
 * Use it to size the buffer passed to pt_span_get_trace_id().
 */
#define PT_TRACE_ID_MAX 128

/* ========================================================================== */
/* Header type                                                                  */
/* ========================================================================== */

/**
 * @brief Logical header group recorded on spans and span events.
 *
 * Mirrors pinpoint::HeaderType.
 */
typedef enum {
    PT_HTTP_REQUEST  = 0, /**< Inbound HTTP request headers. */
    PT_HTTP_RESPONSE = 1, /**< Outbound HTTP response headers. */
    PT_HTTP_COOKIE   = 2  /**< HTTP cookie headers. */
} pt_header_type_t;

/* ========================================================================== */
/* Propagation carrier callback types                                           */
/* ========================================================================== */

/**
 * @brief Look up a value by key in a propagation carrier.
 *
 * @param userdata Opaque pointer provided at carrier construction time.
 * @param key      NUL-terminated header/key name. Built-in HTTP headers are
 *                 queried with mixed-case names ("Pinpoint-TraceID",
 *                 "X-Forwarded-For", ...); configured recorded-header names
 *                 retain their configured spelling. HTTP field names are
 *                 case-insensitive and HTTP/2/3 deliver them lowercase, so an
 *                 HTTP-backed callback MUST match case-insensitively. Only
 *                 non-HTTP carriers with genuinely case-sensitive keys should
 *                 use exact matching.
 * @return         Pointer to the NUL-terminated value, or NULL if absent.
 *                 The pointer must remain valid until the next call on the same
 *                 carrier or until the carrier is destroyed.
 */
typedef const char* (*pt_reader_get_fn)(void* userdata, const char* key);

/**
 * @brief Write a key/value pair into a propagation carrier.
 *
 * @warning @p key and @p value are borrowed pointers that remain valid only
 *          until this callback returns. The callback must consume or copy
 *          their contents before returning and must not retain the pointers.
 *
 * @param userdata Opaque pointer provided at carrier construction time.
 * @param key      NUL-terminated header/key name.
 * @param value    NUL-terminated value to set. Both strings are valid only
 *                 for the duration of the callback — the library may reuse
 *                 the backing buffers for the next call — so copy them if
 *                 they must outlive it.
 */
typedef void (*pt_writer_set_fn)(void* userdata, const char* key, const char* value);

/**
 * @brief Iteration callback invoked by pt_header_for_each_fn for each header.
 *
 * @param key      NUL-terminated header name.
 * @param value    NUL-terminated header value.
 * @param userdata Opaque pointer passed through from the for_each call.
 * @return         0 to continue iteration, non-zero to stop early.
 */
typedef int (*pt_header_foreach_cb)(const char* key, const char* value, void* userdata);

/**
 * @brief Iterate all entries in a header carrier.
 *
 * Implementors call @p callback for each header in iteration order, passing
 * @p callback_userdata through unchanged, until all entries are visited or
 * the callback returns non-zero.
 *
 * @param userdata          Opaque pointer provided at carrier construction time.
 * @param callback          Per-entry callback.
 * @param callback_userdata Opaque pointer forwarded to @p callback.
 */
typedef void (*pt_header_for_each_fn)(void* userdata, pt_header_foreach_cb callback,
                                      void* callback_userdata);

/* ========================================================================== */
/* Propagation carrier structs                                                  */
/* ========================================================================== */

/**
 * @brief Read-only propagation carrier.
 *
 * Mirrors pinpoint::TraceContextReader.  Typically backed by an HTTP request's
 * header map.
 *
 * Example:
 * @code
 *   const char* my_get(void* ud, const char* key) {
 *       return http_header_get((HttpHeaders*)ud, key); // NULL if absent
 *   }
 *   pt_context_reader_t reader = { &my_headers, my_get };
 * @endcode
 */
typedef struct {
    void*            userdata; /**< Caller-managed context passed to callbacks. */
    pt_reader_get_fn get;      /**< Value lookup; must not be NULL. */
} pt_context_reader_t;

/**
 * @brief Write-only propagation carrier.
 *
 * Mirrors pinpoint::TraceContextWriter.  Typically backed by an HTTP request
 * builder's header map.
 *
 * Example:
 * @code
 *   void my_set(void* ud, const char* key, const char* value) {
 *       http_header_set((HttpHeaders*)ud, key, value);
 *   }
 *   pt_context_writer_t writer = { &out_headers, my_set };
 * @endcode
 */
typedef struct {
    void*            userdata; /**< Caller-managed context passed to callbacks. */
    pt_writer_set_fn set;      /**< Value setter; must not be NULL. */
} pt_context_writer_t;

/**
 * @brief Read-only header carrier with iteration support.
 *
 * Mirrors pinpoint::HeaderReader.  Used wherever the library needs to both
 * look up individual headers and iterate over all of them (e.g. recording
 * request headers on a span).
 */
typedef struct {
    void*                 userdata;  /**< Caller-managed context passed to callbacks. */
    pt_reader_get_fn      get;       /**< Value lookup; must not be NULL. */
    pt_header_for_each_fn for_each;  /**< Full iteration; must not be NULL. */
} pt_header_reader_t;

/**
 * @brief Read/write header carrier with iteration support.
 *
 * Mirrors pinpoint::HeaderReaderWriter.  Convenience type for situations where
 * a single carrier supports both reading and writing (e.g. an in-place header
 * map used for context propagation).
 *
 * To pass this carrier to a function that accepts pt_context_writer_t, build a
 * separate pt_context_writer_t with the same userdata and set callback.
 */
typedef struct {
    void*                 userdata;  /**< Caller-managed context passed to callbacks. */
    pt_reader_get_fn      get;       /**< Value lookup; must not be NULL. */
    pt_header_for_each_fn for_each;  /**< Full iteration; must not be NULL. */
    pt_writer_set_fn      set;       /**< Value setter; must not be NULL. */
} pt_header_reader_writer_t;

/* ========================================================================== */
/* Call stack reader                                                            */
/* ========================================================================== */

/**
 * @brief Per-frame callback invoked during call stack iteration.
 *
 * @param module    NUL-terminated module/library name (may be empty, never NULL).
 * @param function  NUL-terminated function/symbol name.
 * @param file      NUL-terminated source file path (may be empty, never NULL).
 * @param line      Source line number (0 if unavailable).
 * @param userdata  Opaque pointer passed through from the for_each call.
 */
typedef void (*pt_callstack_frame_cb)(const char* module, const char* function,
                                      const char* file, int line, void* userdata);

/**
 * @brief Call stack provider passed to error-recording functions.
 *
 * Mirrors pinpoint::CallStackReader.
 */
typedef struct {
    void* userdata; /**< Caller-managed context passed to callbacks. */
    /** Iterate all frames, invoking @p callback for each. */
    void (*for_each)(void* userdata, pt_callstack_frame_cb callback, void* callback_userdata);
} pt_callstack_reader_t;

/* ========================================================================== */
/* Agent options                                                                */
/* ========================================================================== */

/**
 * @brief Opaque builder for pt_start_agent() inputs.
 *
 * Mirrors pinpoint::AgentOptions. Build one with pt_agent_options_new(),
 * populate it with the setters below, pass it to pt_start_agent() and release
 * it with pt_agent_options_free(). The options object is only read during the
 * pt_start_agent() call; it may be freed immediately afterwards.
 */
typedef struct pt_agent_options_s* pt_agent_options_t;

/**
 * @brief Allocates an options object with all defaults. Returns NULL only if
 *        allocation fails.
 */
pt_agent_options_t pt_agent_options_new(void);

/**
 * @brief Releases an options object.
 *
 * NULL is ignored. Options handles are registry tokens like agent and span
 * handles: a second free of the same handle, or any setter/pt_start_agent()
 * call made with an already-freed handle, is ignored.
 */
void pt_agent_options_free(pt_agent_options_t options);

/**
 * @brief Sets the YAML configuration file path.
 *
 * Precedence: the `<env_prefix>_CONFIG_FILE` environment variable overrides
 * this path; a configured file's content replaces the YAML string set with
 * pt_agent_options_set_config_yaml() wholesale on every (re)load — the two
 * sources are never merged. Environment variables still override individual
 * settings from either source. A NULL value resets to "not set".
 */
void pt_agent_options_set_config_file(pt_agent_options_t options, const char* path);

/**
 * @brief Injects a YAML configuration string, used when no config file is
 *        configured (see pt_agent_options_set_config_file()). A NULL value
 *        resets to "not set".
 */
void pt_agent_options_set_config_yaml(pt_agent_options_t options, const char* yaml);

/**
 * @brief Overrides the prefix of the environment variable names the agent
 *        reads (e.g. "MYAPP" makes it read MYAPP_APPLICATION_NAME). NULL or
 *        empty selects the default "PINPOINT_CPP".
 */
void pt_agent_options_set_env_prefix(pt_agent_options_t options, const char* prefix);

/** @brief Sets the application type reported to the collector. */
void pt_agent_options_set_app_type(pt_agent_options_t options, int32_t app_type);

/**
 * @brief Destination for the agent's own log lines, set with
 *        pt_agent_options_set_log_sink().
 *
 * @param userdata The pointer handed to pt_agent_options_set_log_sink().
 * @param level    One of "debug", "info", "warning", "error".
 * @param message  The formatted line, "[pinpoint][file:line] text", with no
 *                 trailing newline and no timestamp — the host's logger stamps
 *                 its own.
 *
 * Both strings are owned by the agent and valid only for the duration of the
 * call; copy anything the sink keeps.
 *
 * Contract:
 * - NOT reentrant: the sink runs while the agent holds its logger mutex, so
 *   calling any pt_* function from it deadlocks the calling thread.
 * - Must not block: it runs inline on whichever thread logged, including host
 *   request threads and the agent's gRPC workers.
 * - Must be thread-safe: several threads call it concurrently.
 * - A C++ exception escaping it (for a sink implemented in C++) drops the line
 *   and nothing else.
 */
typedef void (*pt_log_sink_fn)(void* userdata, const char* level, const char* message);

/**
 * @brief Routes the agent's log lines to the host's own log pipeline
 *        (nginx error_log, an application logger) instead of the built-in
 *        sinks.
 *
 * When a sink is set, nothing goes to `Log.FilePath` or to stdout, so lines
 * are not duplicated. The sink is installed by pt_start_agent() before the
 * configuration is parsed — configuration errors reach it too — and dropped
 * again by pt_agent_shutdown(), after which the host's logger may be torn
 * down. A NULL @p sink clears it.
 *
 * @param userdata Opaque pointer passed back to every call. Not owned by the
 *                 agent; it must outlive the agent, or at least the last log
 *                 line, which is the pt_agent_shutdown() that clears the sink.
 */
void pt_agent_options_set_log_sink(pt_agent_options_t options,
                                   pt_log_sink_fn sink,
                                   void* userdata);

/**
 * @brief Sets AgentInfo server metadata.
 *
 * @param server_info Server runtime description. NULL keeps the default.
 * @param args        Optional array of command line argument strings. May be NULL.
 * @param args_count  Number of entries in args. Values <= 0 are treated as 0.
 * @param libs        Optional array of service library strings. May be NULL.
 * @param libs_count  Number of entries in libs. Values <= 0 are treated as 0.
 */
void pt_agent_options_set_server_metadata(pt_agent_options_t options,
                                          const char* server_info,
                                          const char* const* args,
                                          int args_count,
                                          const char* const* libs,
                                          int libs_count);

/* ========================================================================== */
/* Agent lifecycle                                                              */
/* ========================================================================== */

/**
 * @brief Creates, configures and starts the agent in the CURRENT process, and
 *        installs it as the global agent.
 *
 * This is the only way to bring an agent online. Call it in the process that
 * will record spans — for pre-fork servers (nginx, Apache prefork, ...) that
 * means each worker calls pt_start_agent() after fork(), from its worker-init
 * hook, and the master process makes NO agent API calls at all. Each worker
 * registers as its own agent instance with a process-unique agent id.
 *
 * The call returns as soon as initialization is launched; it does NOT wait
 * for collector registration. pt_agent_is_enabled() flips to non-zero once
 * registration succeeds.
 *
 * @return Non-zero when the agent was launched and installed as the global
 * agent — obtain the handle with pt_global_agent(). 0 on a configuration or
 * setup failure; check the agent log for the cause. Nothing is installed as
 * the global agent then — pt_global_agent() keeps returning a disabled
 * (noop) agent handle — and a later pt_start_agent() call retries from
 * scratch.
 *
 * Calling pt_start_agent() again in the same process leaves the already
 * running agent untouched and reports success (with a warning). After
 * pt_agent_shutdown() a new pt_start_agent() call builds a fresh agent.
 *
 * An agent inherited across fork() is unusable in the child and is refused
 * (returns 0) — see pinpoint::StartAgent().
 *
 * @param options Options built with pt_agent_options_new(); NULL uses all
 *                defaults. Only read during this call.
 *
 * Mirrors pinpoint::StartAgent().
 */
int pt_start_agent(pt_agent_options_t options);

/**
 * @brief Returns a handle to the singleton global agent.
 *
 * For a live global agent, each call returns a distinct owning handle that
 * should be released with pt_agent_destroy(). Destroying it only releases the
 * C wrapper; the global agent itself is managed internally. If no global
 * agent exists, calls share a disabled-agent sentinel whose destroy operation
 * is a no-op. NULL is returned only if handle setup itself fails.
 *
 * Mirrors pinpoint::GlobalAgent().
 */
pt_agent_t pt_global_agent(void);

/**
 * @brief Releases an agent handle obtained from pt_global_agent().
 *
 * This only frees the C wrapper; the global agent itself is unaffected.
 * See "Handle validity" at the top of this header.
 */
void pt_agent_destroy(pt_agent_t agent);

/**
 * @brief Returns non-zero after initialization succeeds and tracing is
 *        enabled. Individual spans may still be rejected by sampling.
 *        Always 0 for an agent handle inherited across fork().
 *
 * Mirrors pinpoint::Agent::Enable().
 */
int pt_agent_is_enabled(pt_agent_t agent);

/**
 * @brief Stops the agent and waits for worker shutdown.
 *
 * Queued spans are submitted when a collector channel is available, but
 * successful delivery is not guaranteed.
 *
 * Terminal for this agent: it can never be brought back online,
 * pt_agent_is_enabled() stays 0 and it only produces noop spans. When it is
 * the global agent it is also removed from the singleton, so pt_global_agent()
 * returns the disabled-agent sentinel until a new one is installed.
 *
 * To resume tracing in the same process, release the handle with
 * pt_agent_destroy() and call pt_start_agent() again; pt_global_agent() then
 * hands out the fresh agent. Each such cycle re-resolves the agent identity,
 * so it registers a NEW agent instance with the collector.
 *
 * Mirrors pinpoint::Agent::Shutdown().
 */
void pt_agent_shutdown(pt_agent_t agent);

/* ========================================================================== */
/* Span creation                                                                */
/* ========================================================================== */

/**
 * @brief Creates a new outbound span (no incoming context).
 *
 * The returned handle must be released with pt_span_destroy() after
 * pt_span_end() has been called.
 *
 * Mirrors pinpoint::Agent::NewSpan(operation, rpc_point).
 */
pt_span_t pt_agent_new_span(pt_agent_t agent, const char* operation, const char* rpc_point);

/**
 * @brief Creates a new span, extracting context from an inbound carrier.
 *
 * @param reader  Inbound propagation carrier (e.g. HTTP request headers).
 *                May be NULL, in which case no context is extracted.
 *
 * Mirrors pinpoint::Agent::NewSpan(operation, rpc_point, reader).
 */
pt_span_t pt_agent_new_span_with_reader(pt_agent_t agent, const char* operation,
                                        const char* rpc_point,
                                        const pt_context_reader_t* reader);

/**
 * @brief Creates a new span with an HTTP method and inbound context.
 *
 * @param method  NUL-terminated HTTP verb (e.g. "GET", "POST").
 * @param reader  Inbound propagation carrier.  May be NULL.
 *
 * Mirrors pinpoint::Agent::NewSpan(operation, rpc_point, method, reader).
 */
pt_span_t pt_agent_new_span_with_method(pt_agent_t agent, const char* operation,
                                        const char* rpc_point, const char* method,
                                        const pt_context_reader_t* reader);

/* ========================================================================== */
/* Span operations                                                              */
/* ========================================================================== */

/**
 * @brief Releases a span handle.
 *
 * Call pt_span_end() first to finalize and enqueue the span data, then destroy
 * the handle. See "Handle validity" at the top of this header.
 */
void pt_span_destroy(pt_span_t span);

/**
 * @brief Creates a new child span event and pushes it onto the event stack.
 *
 * The returned non-owning handle remains valid while the parent span is alive.
 * Call pt_span_event_end() to pop and finalize the event; later recording
 * calls become warning no-ops. pt_span_event_destroy() is a compatibility
 * no-op and does not extend the lifetime.
 *
 * Mirrors pinpoint::Span::NewSpanEvent(operation).
 */
pt_span_event_t pt_span_new_event(pt_span_t span, const char* operation);

/** Mirrors pinpoint::Span::NewSpanEvent(operation, service_type). */
pt_span_event_t pt_span_new_event_with_type(pt_span_t span, const char* operation,
                                            int32_t service_type);

/**
 * @brief Returns the current (top-of-stack) span event.
 *
 * The returned non-owning handle remains valid while the parent span is alive.
 * Once the event or span is finalized, recording calls become no-ops.
 * pt_span_event_destroy() is a compatibility no-op.
 *
 * When there is no active event (the span is already ended, or no event is on
 * the stack), this returns a valid handle to a shared no-op event that
 * silently ignores recording, so the return value cannot be used to detect
 * whether an event is active. NULL is returned when `span` is NULL or already
 * destroyed, or when an internal failure is caught at the C API boundary.
 *
 * Mirrors pinpoint::Span::GetSpanEvent().
 */
pt_span_event_t pt_span_get_event(pt_span_t span);

/** Mirrors pinpoint::Span::EndSpan(). */
void pt_span_end(pt_span_t span);

/**
 * @brief Creates an asynchronous child span for background operations.
 *
 * The returned handle must be released with pt_span_destroy().
 *
 * Mirrors pinpoint::Span::NewAsyncSpan(async_operation).
 */
pt_span_t pt_span_new_async_span(pt_span_t span, const char* async_operation);

/**
 * @brief Writes the span's distributed trace identifier, in its wire form
 *        `"<agent_id>^<start_time>^<sequence>"`, into the caller's buffer.
 *
 * Follows snprintf() conventions: when @p buf_size > 0 at most
 * @p buf_size - 1 bytes are copied followed by a NUL terminator, and the
 * return value is the full trace-id length excluding the NUL. A return value
 * >= @p buf_size therefore means the output was truncated. Passing
 * @p buf_size == 0 (or @p buf == NULL) writes nothing and only reports the
 * length needed, so callers can size a buffer with a first zero-length call.
 * PT_TRACE_ID_MAX is a buffer size that never truncates.
 *
 * @return Length of the trace id in bytes, excluding the NUL terminator; 0 for
 *         a span that carries no trace (e.g. a noop/unsampled span). On such a
 *         result the buffer is set to an empty string when @p buf_size > 0.
 *
 * Mirrors pinpoint::Span::GetTraceId().
 */
size_t pt_span_get_trace_id(pt_span_t span, char* buf, size_t buf_size);

/** Mirrors pinpoint::Span::GetSpanId(). */
int64_t pt_span_get_span_id(pt_span_t span);

/** Mirrors pinpoint::Span::IsSampled(). */
int pt_span_is_sampled(pt_span_t span);

/** Mirrors pinpoint::Span::SetServiceType(). */
void pt_span_set_service_type(pt_span_t span, int32_t service_type);

/**
 * @brief Sets the span start time.
 *
 * @param ms_since_epoch  Start time expressed as milliseconds since the Unix
 *                        epoch (UTC). The unit matters: elapsed times are
 *                        reported as 32-bit millisecond deltas on the wire,
 *                        so a value in seconds (e.g. time(NULL)) makes the
 *                        computed deltas overflow int32 and silently corrupts
 *                        the trace timeline. The value is not validated.
 *
 * Mirrors pinpoint::Span::SetStartTime().
 */
void pt_span_set_start_time_ms(pt_span_t span, int64_t ms_since_epoch);

/** Mirrors pinpoint::Span::SetRemoteAddress(). */
void pt_span_set_remote_address(pt_span_t span, const char* address);

/** Mirrors pinpoint::Span::SetEndPoint(). */
void pt_span_set_end_point(pt_span_t span, const char* end_point);

/** Mirrors pinpoint::Span::SetAcceptorHost(). */
void pt_span_set_acceptor_host(pt_span_t span, const char* host);

/** Mirrors pinpoint::Span::SetError(error_message). */
void pt_span_set_error(pt_span_t span, const char* error_message);

/** Mirrors pinpoint::Span::SetError(error_name, error_message). */
void pt_span_set_error_named(pt_span_t span, const char* error_name,
                             const char* error_message);

/** Mirrors pinpoint::Span::SetStatusCode(). */
void pt_span_set_status_code(pt_span_t span, int status_code);

/** Mirrors pinpoint::Span::SetUrlStat(). */
void pt_span_set_url_stat(pt_span_t span, const char* url_pattern,
                          const char* method, int status_code);

/** Mirrors pinpoint::Span::SetLogging(). */
void pt_span_set_logging(pt_span_t span, pt_context_writer_t* writer);

/** Mirrors pinpoint::Span::RecordHeader(). */
void pt_span_record_header(pt_span_t span, pt_header_type_t which,
                           const pt_header_reader_t* reader);

/**
 * @brief Records an integer annotation on the span.
 *
 * Recording on an already-ended span is a warning no-op (the same holds for
 * the other pt_span_set_annotation_* functions).
 *
 * Mirrors pinpoint::Span::SetAnnotation() with an int32_t value.
 */
void pt_span_set_annotation_int(pt_span_t span, int32_t key, int32_t value);

/** Mirrors pinpoint::Span::SetAnnotation() with an int64_t value. */
void pt_span_set_annotation_long(pt_span_t span, int32_t key, int64_t value);

/** Mirrors pinpoint::Span::SetAnnotation() with a string value. */
void pt_span_set_annotation_string(pt_span_t span, int32_t key, const char* value);

/** Mirrors pinpoint::Span::SetAnnotation() with a string-pair value. */
void pt_span_set_annotation_string_string(pt_span_t span, int32_t key,
                                          const char* value1, const char* value2);

/* ========================================================================== */
/* SpanEvent operations                                                         */
/* ========================================================================== */

/**
 * @brief Compatibility no-op for a span event view handle.
 *
 * Span-event handles are non-owning pointer views. This function does not
 * end/finalize the event, free memory, or extend the event lifetime.
 */
void pt_span_event_destroy(pt_span_event_t se);

/**
 * @brief Pops and finalizes this span event.
 *
 * Records the elapsed time and removes the event from the parent span's event
 * stack. Ending an already-ended event is a warning no-op. The event handle
 * remains valid while the parent span is alive, but further recording calls
 * are no-ops; pt_span_event_destroy() is an optional compatibility no-op.
 *
 * Mirrors pinpoint::SpanEvent::EndEvent().
 */
void pt_span_event_end(pt_span_event_t se);

/** Mirrors pinpoint::SpanEvent::SetServiceType(). */
void pt_span_event_set_service_type(pt_span_event_t se, int32_t service_type);

/** Mirrors pinpoint::SpanEvent::SetOperationName(). */
void pt_span_event_set_operation_name(pt_span_event_t se, const char* operation);

/**
 * @brief Sets the span event start time.
 *
 * @param ms_since_epoch  Start time as milliseconds since the Unix epoch
 *                        (UTC). The unit matters: elapsed/offset fields are
 *                        reported as 32-bit millisecond deltas on the wire,
 *                        so a value in seconds (e.g. time(NULL)) makes the
 *                        computed deltas overflow int32 and silently corrupts
 *                        the trace timeline. The value is not validated.
 *
 * Mirrors pinpoint::SpanEvent::SetStartTime().
 */
void pt_span_event_set_start_time_ms(pt_span_event_t se, int64_t ms_since_epoch);

/** Mirrors pinpoint::SpanEvent::SetDestination(). */
void pt_span_event_set_destination(pt_span_event_t se, const char* dest);

/** Mirrors pinpoint::SpanEvent::SetEndPoint(). */
void pt_span_event_set_end_point(pt_span_event_t se, const char* end_point);

/** Mirrors pinpoint::SpanEvent::SetError(error_message). */
void pt_span_event_set_error(pt_span_event_t se, const char* error_message);

/** Mirrors pinpoint::SpanEvent::SetError(error_name, error_message). */
void pt_span_event_set_error_named(pt_span_event_t se, const char* error_name,
                                   const char* error_message);

/**
 * @brief Records a named error along with a call stack.
 *
 * @param reader  Call stack provider.  May be NULL.
 *
 * Mirrors pinpoint::SpanEvent::SetError(error_name, error_message, reader).
 */
void pt_span_event_set_error_with_callstack(pt_span_event_t se,
                                            const char* error_name,
                                            const char* error_message,
                                            const pt_callstack_reader_t* reader);

/**
 * @brief Records a SQL query and its bound parameters.
 *
 * @param args        Array of null-terminated parameter strings. May be NULL
 *                    when args_count is 0. A NULL array element is recorded
 *                    as the SQL null value.
 * @param args_count  Number of entries in args.
 *
 * Mirrors pinpoint::SpanEvent::SetSqlQuery().
 */
void pt_span_event_set_sql_query(pt_span_event_t se, const char* sql_query,
                                 const char* const* args, size_t args_count);

/** Mirrors pinpoint::SpanEvent::RecordHeader(). */
void pt_span_event_record_header(pt_span_event_t se, pt_header_type_t which,
                                 const pt_header_reader_t* reader);

/**
 * @brief Injects the trace context for the outbound call represented by this
 *        span event into an outbound carrier.
 *
 * Mirrors pinpoint::SpanEvent::InjectContext(writer).
 */
void pt_span_event_inject_context(pt_span_event_t se, pt_context_writer_t* writer);

/**
 * @brief Records an integer annotation on the span event.
 *
 * Recording on an already-ended event is a warning no-op (the same holds for
 * the other pt_span_event_set_annotation_* functions).
 *
 * Mirrors pinpoint::SpanEvent::SetAnnotation() with an int32_t value.
 */
void pt_span_event_set_annotation_int(pt_span_event_t se, int32_t key, int32_t value);

/** Mirrors pinpoint::SpanEvent::SetAnnotation() with an int64_t value. */
void pt_span_event_set_annotation_long(pt_span_event_t se, int32_t key, int64_t value);

/** Mirrors pinpoint::SpanEvent::SetAnnotation() with a string value. */
void pt_span_event_set_annotation_string(pt_span_event_t se, int32_t key,
                                         const char* value);

/** Mirrors pinpoint::SpanEvent::SetAnnotation() with a string-pair value. */
void pt_span_event_set_annotation_string_string(pt_span_event_t se, int32_t key,
                                                const char* value1, const char* value2);

/* ========================================================================== */
/* HTTP helper functions                                                        */
/* ========================================================================== */

/**
 * Each function below mirrors the pinpoint::helper:: overload of the matching
 * name; see pinpoint/tracer.h for what each one records. A NULL handle or
 * carrier makes the call a no-op; a NULL string argument is recorded as "".
 */

/** Mirrors pinpoint::helper::TraceHttpServerRequest(). */
void pt_trace_http_server_request(pt_span_t span,
                                  const char* remote_addr,
                                  const char* endpoint,
                                  const pt_header_reader_t* request_reader);

/** Mirrors the TraceHttpServerRequest() overload taking a cookie reader. */
void pt_trace_http_server_request_with_cookie(pt_span_t span,
                                              const char* remote_addr,
                                              const char* endpoint,
                                              const pt_header_reader_t* request_reader,
                                              const pt_header_reader_t* cookie_reader);

/** Mirrors pinpoint::helper::TraceHttpServerResponse(). */
void pt_trace_http_server_response(pt_span_t span,
                                   const char* url_pattern,
                                   const char* method,
                                   int status_code,
                                   const pt_header_reader_t* response_reader);

/** Mirrors pinpoint::helper::TraceHttpClientRequest(). */
void pt_trace_http_client_request(pt_span_event_t se,
                                  const char* host,
                                  const char* url,
                                  const pt_header_reader_t* request_reader);

/** Mirrors the TraceHttpClientRequest() overload taking a cookie reader. */
void pt_trace_http_client_request_with_cookie(pt_span_event_t se,
                                              const char* host,
                                              const char* url,
                                              const pt_header_reader_t* request_reader,
                                              const pt_header_reader_t* cookie_reader);

/** Mirrors pinpoint::helper::TraceHttpClientResponse(). */
void pt_trace_http_client_response(pt_span_event_t se,
                                   int status_code,
                                   const pt_header_reader_t* response_reader);

#ifdef __cplusplus
}
#endif

#endif /* PINPOINT_TRACER_C_H */
