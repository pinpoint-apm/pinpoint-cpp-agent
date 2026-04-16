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
 *   // Optional: load configuration before creating the agent.
 *   pt_set_config_file_path("/etc/pinpoint/agent.yaml");
 *
 *   pt_agent_t agent = pt_create_agent();
 *
 *   // --- incoming request ---
 *   pt_context_reader_t reader = { &my_headers, my_header_get };
 *   pt_span_t span = pt_agent_new_span_with_reader(agent, "MyService", "/api/v1", &reader);
 *
 *   // create a child event
 *   pt_span_event_t se = pt_span_new_event(span, "db_query");
 *   pt_span_event_set_service_type(se, PT_SERVICE_TYPE_MYSQL_QUERY);
 *   pt_annotation_t anno = pt_span_event_get_annotations(se);
 *   pt_annotation_append_string(anno, PT_ANNOTATION_HTTP_URL, "/api/v1");
 *   pt_annotation_destroy(anno);
 *   pt_span_end_event(span);
 *   pt_span_event_destroy(se);
 *
 *   pt_span_end(span);
 *   pt_span_destroy(span);
 *
 *   pt_agent_shutdown(agent);
 *   pt_agent_destroy(agent);
 * @endcode
 */

#ifndef PINPOINT_TRACER_C_H
#define PINPOINT_TRACER_C_H

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
/** Opaque handle to a span event (child operation). */
typedef struct pt_span_event_s* pt_span_event_t;
/** Opaque handle to the annotation container of a span or span event. */
typedef struct pt_annotation_s* pt_annotation_t;

/* ========================================================================== */
/* Trace identifier                                                             */
/* ========================================================================== */

/** Maximum length (including NUL terminator) of the agent-id string. */
#define PT_AGENT_ID_MAX 256

/**
 * @brief Distributed trace identifier.
 *
 * Mirrors pinpoint::TraceId.  The wire representation is
 * `"<agent_id>^<start_time>^<sequence>"`.
 */
typedef struct {
    char    agent_id[PT_AGENT_ID_MAX]; /**< NUL-terminated agent identifier. */
    int64_t start_time;                /**< Agent start time (milliseconds since epoch). */
    int64_t sequence;                  /**< Per-agent sequence number. */
} pt_trace_id_t;

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
 * @param key      NUL-terminated, case-sensitive header/key name.
 * @return         Pointer to the NUL-terminated value, or NULL if absent.
 *                 The pointer must remain valid until the next call on the same
 *                 carrier or until the carrier is destroyed.
 */
typedef const char* (*pt_reader_get_fn)(void* userdata, const char* key);

/**
 * @brief Write a key/value pair into a propagation carrier.
 *
 * @param userdata Opaque pointer provided at carrier construction time.
 * @param key      NUL-terminated header/key name.
 * @param value    NUL-terminated value to set.
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
 * Implementors must call @p callback once for every header, passing
 * @p callback_userdata through unchanged.  Iteration stops when @p callback
 * returns non-zero.
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
/* Global configuration                                                         */
/* ========================================================================== */

/**
 * @brief Sets the path of the YAML configuration file.
 *
 * Must be called before pt_create_agent() / pt_global_agent().
 * Mirrors pinpoint::SetConfigFilePath().
 */
void pt_set_config_file_path(const char* config_file_path);

/**
 * @brief Injects a YAML configuration string directly.
 *
 * Must be called before pt_create_agent() / pt_global_agent().
 * Mirrors pinpoint::SetConfigString().
 */
void pt_set_config_string(const char* config_string);

/* ========================================================================== */
/* Agent lifecycle                                                              */
/* ========================================================================== */

/**
 * @brief Creates a new Pinpoint agent using the global configuration.
 *
 * The returned handle must be released with pt_agent_destroy() after
 * pt_agent_shutdown() has been called.
 *
 * Mirrors pinpoint::CreateAgent().
 */
pt_agent_t pt_create_agent(void);

/**
 * @brief Creates a new Pinpoint agent with an explicit application type.
 *
 * @param app_type  Application service-type constant (e.g. PT_APP_TYPE_CPP).
 *
 * Mirrors pinpoint::CreateAgent(int32_t).
 */
pt_agent_t pt_create_agent_with_type(int32_t app_type);

/**
 * @brief Returns a handle to the singleton global agent.
 *
 * The returned handle must NOT be passed to pt_agent_destroy() — its lifetime
 * is managed internally.  Returns NULL if no global agent exists.
 *
 * Mirrors pinpoint::GlobalAgent().
 */
pt_agent_t pt_global_agent(void);

/**
 * @brief Releases an agent handle created by pt_create_agent() or
 *        pt_create_agent_with_type().
 *
 * @warning Do NOT call this on a handle obtained from pt_global_agent().
 */
void pt_agent_destroy(pt_agent_t agent);

/**
 * @brief Returns non-zero if the agent is enabled and actively sampling.
 *
 * Mirrors pinpoint::Agent::Enable().
 */
int pt_agent_is_enabled(pt_agent_t agent);

/**
 * @brief Initiates a graceful shutdown and flushes pending spans.
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
 * Call pt_span_end() first to flush the span data, then destroy the handle.
 */
void pt_span_destroy(pt_span_t span);

/**
 * @brief Creates a new child span event and pushes it onto the event stack.
 *
 * The returned handle must be released with pt_span_event_destroy().
 * Call pt_span_end_event() on the *span* (not on the event handle) to pop and
 * finalize the event.
 *
 * Mirrors pinpoint::Span::NewSpanEvent(operation).
 */
pt_span_event_t pt_span_new_event(pt_span_t span, const char* operation);

/**
 * @brief Creates a new child span event with an explicit service type.
 *
 * Mirrors pinpoint::Span::NewSpanEvent(operation, service_type).
 */
pt_span_event_t pt_span_new_event_with_type(pt_span_t span, const char* operation,
                                            int32_t service_type);

/**
 * @brief Returns the current (top-of-stack) span event.
 *
 * The returned handle must be released with pt_span_event_destroy().
 * Returns NULL if there is no active event.
 *
 * Mirrors pinpoint::Span::GetSpanEvent().
 */
pt_span_event_t pt_span_get_event(pt_span_t span);

/**
 * @brief Pops and finalizes the current span event.
 *
 * Records the elapsed time and removes the event from the event stack.
 * The span event handle obtained from pt_span_new_event() remains valid
 * after this call but should be released with pt_span_event_destroy().
 *
 * Mirrors pinpoint::Span::EndSpanEvent().
 */
void pt_span_end_event(pt_span_t span);

/**
 * @brief Completes the span and flushes all recorded data to the collector.
 *
 * Mirrors pinpoint::Span::EndSpan().
 */
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
 * @brief Injects the current span context into an outbound carrier.
 *
 * Mirrors pinpoint::Span::InjectContext(writer).
 */
void pt_span_inject_context(pt_span_t span, pt_context_writer_t* writer);

/**
 * @brief Applies trace context from an inbound carrier to the span.
 *
 * Mirrors pinpoint::Span::ExtractContext(reader).
 */
void pt_span_extract_context(pt_span_t span, const pt_context_reader_t* reader);

/**
 * @brief Returns the distributed trace identifier for this span.
 *
 * The returned structure is a value copy and is valid independently of the
 * span's lifetime.
 *
 * Mirrors pinpoint::Span::GetTraceId().
 */
pt_trace_id_t pt_span_get_trace_id(pt_span_t span);

/**
 * @brief Returns the numeric span identifier.
 *
 * Mirrors pinpoint::Span::GetSpanId().
 */
int64_t pt_span_get_span_id(pt_span_t span);

/**
 * @brief Returns non-zero if this span is being sampled.
 *
 * Mirrors pinpoint::Span::IsSampled().
 */
int pt_span_is_sampled(pt_span_t span);

/** Mirrors pinpoint::Span::SetServiceType(). */
void pt_span_set_service_type(pt_span_t span, int32_t service_type);

/**
 * @brief Sets the span start time.
 *
 * @param ms_since_epoch  Start time expressed as milliseconds since the Unix
 *                        epoch (UTC).
 *
 * Mirrors pinpoint::Span::SetStartTime().
 */
void pt_span_set_start_time_ms(pt_span_t span, int64_t ms_since_epoch);

/** Mirrors pinpoint::Span::SetRemoteAddress(). */
void pt_span_set_remote_address(pt_span_t span, const char* address);

/** Mirrors pinpoint::Span::SetEndPoint(). */
void pt_span_set_end_point(pt_span_t span, const char* end_point);

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
 * @brief Returns the annotation container for this span.
 *
 * The returned handle must be released with pt_annotation_destroy() when no
 * longer needed.
 *
 * Mirrors pinpoint::Span::GetAnnotations().
 */
pt_annotation_t pt_span_get_annotations(pt_span_t span);

/* ========================================================================== */
/* SpanEvent operations                                                         */
/* ========================================================================== */

/**
 * @brief Releases a span event handle.
 *
 * Does NOT end/finalize the event — call pt_span_end_event() on the parent
 * span first.
 */
void pt_span_event_destroy(pt_span_event_t se);

/** Mirrors pinpoint::SpanEvent::SetServiceType(). */
void pt_span_event_set_service_type(pt_span_event_t se, int32_t service_type);

/** Mirrors pinpoint::SpanEvent::SetOperationName(). */
void pt_span_event_set_operation_name(pt_span_event_t se, const char* operation);

/**
 * @brief Sets the span event start time.
 *
 * @param ms_since_epoch  Start time as milliseconds since the Unix epoch (UTC).
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

/** Mirrors pinpoint::SpanEvent::SetSqlQuery(). */
void pt_span_event_set_sql_query(pt_span_event_t se, const char* sql_query,
                                 const char* args);

/** Mirrors pinpoint::SpanEvent::RecordHeader(). */
void pt_span_event_record_header(pt_span_event_t se, pt_header_type_t which,
                                 const pt_header_reader_t* reader);

/**
 * @brief Returns the annotation container for this span event.
 *
 * The returned handle must be released with pt_annotation_destroy().
 *
 * Mirrors pinpoint::SpanEvent::GetAnnotations().
 */
pt_annotation_t pt_span_event_get_annotations(pt_span_event_t se);

/* ========================================================================== */
/* Annotation operations                                                        */
/* ========================================================================== */

/**
 * @brief Releases an annotation handle obtained from pt_span_get_annotations()
 *        or pt_span_event_get_annotations().
 */
void pt_annotation_destroy(pt_annotation_t anno);

/** Mirrors pinpoint::Annotation::AppendInt(). */
void pt_annotation_append_int(pt_annotation_t anno, int32_t key, int32_t value);

/** Mirrors pinpoint::Annotation::AppendLong(). */
void pt_annotation_append_long(pt_annotation_t anno, int32_t key, int64_t value);

/** Mirrors pinpoint::Annotation::AppendString(). */
void pt_annotation_append_string(pt_annotation_t anno, int32_t key, const char* value);

/** Mirrors pinpoint::Annotation::AppendStringString(). */
void pt_annotation_append_string_string(pt_annotation_t anno, int32_t key,
                                        const char* s1, const char* s2);

/** Mirrors pinpoint::Annotation::AppendIntStringString(). */
void pt_annotation_append_int_string_string(pt_annotation_t anno, int32_t key,
                                            int i, const char* s1, const char* s2);

/**
 * @brief Mirrors pinpoint::Annotation::AppendBytesStringString().
 *
 * @param uid      Pointer to binary data.
 * @param uid_len  Length of @p uid in bytes.
 */
void pt_annotation_append_bytes_string_string(pt_annotation_t anno, int32_t key,
                                              const unsigned char* uid, int uid_len,
                                              const char* s1, const char* s2);

/** Mirrors pinpoint::Annotation::AppendLongIntIntByteByteString(). */
void pt_annotation_append_long_int_int_byte_byte_string(pt_annotation_t anno, int32_t key,
                                                        int64_t l,
                                                        int32_t i1, int32_t i2,
                                                        int32_t b1, int32_t b2,
                                                        const char* s);

/* ========================================================================== */
/* HTTP helper functions                                                        */
/* ========================================================================== */

/**
 * @brief Records an HTTP server request on the span.
 *
 * Mirrors pinpoint::helper::TraceHttpServerRequest(span, remote_addr, endpoint,
 *                                                  request_reader).
 */
void pt_trace_http_server_request(pt_span_t span,
                                  const char* remote_addr,
                                  const char* endpoint,
                                  const pt_header_reader_t* request_reader);

/**
 * @brief Records an HTTP server request with cookies on the span.
 *
 * Mirrors pinpoint::helper::TraceHttpServerRequest(span, remote_addr, endpoint,
 *                                                  request_reader, cookie_reader).
 */
void pt_trace_http_server_request_with_cookie(pt_span_t span,
                                              const char* remote_addr,
                                              const char* endpoint,
                                              const pt_header_reader_t* request_reader,
                                              const pt_header_reader_t* cookie_reader);

/**
 * @brief Records an HTTP server response on the span.
 *
 * Mirrors pinpoint::helper::TraceHttpServerResponse().
 */
void pt_trace_http_server_response(pt_span_t span,
                                   const char* url_pattern,
                                   const char* method,
                                   int status_code,
                                   const pt_header_reader_t* response_reader);

/**
 * @brief Records an HTTP client request on the span event.
 *
 * Mirrors pinpoint::helper::TraceHttpClientRequest(span_event, host, url,
 *                                                  request_reader).
 */
void pt_trace_http_client_request(pt_span_event_t se,
                                  const char* host,
                                  const char* url,
                                  const pt_header_reader_t* request_reader);

/**
 * @brief Records an HTTP client request with cookies on the span event.
 *
 * Mirrors pinpoint::helper::TraceHttpClientRequest(span_event, host, url,
 *                                                  request_reader, cookie_reader).
 */
void pt_trace_http_client_request_with_cookie(pt_span_event_t se,
                                              const char* host,
                                              const char* url,
                                              const pt_header_reader_t* request_reader,
                                              const pt_header_reader_t* cookie_reader);

/**
 * @brief Records an HTTP client response on the span event.
 *
 * Mirrors pinpoint::helper::TraceHttpClientResponse().
 */
void pt_trace_http_client_response(pt_span_event_t se,
                                   int status_code,
                                   const pt_header_reader_t* response_reader);

#ifdef __cplusplus
}
#endif

#endif /* PINPOINT_TRACER_C_H */
