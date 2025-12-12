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
 * @file pinpoint_c.h
 * @brief C wrapper for Pinpoint C++ Agent
 *
 * This header provides C-compatible functions to use the Pinpoint C++ agent
 * from C applications. All C++ objects are accessed through opaque handles.
 */

#ifndef PINPOINT_C_H
#define PINPOINT_C_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Export Macros
 * ============================================================================= */

#if defined(_WIN32) || defined(_WIN64)
    #ifdef PINPOINT_C_EXPORTS
        #define PINPOINT_C_API __declspec(dllexport)
    #else
        #define PINPOINT_C_API __declspec(dllimport)
    #endif
#else
    #ifdef PINPOINT_C_EXPORTS
        #define PINPOINT_C_API __attribute__((visibility("default")))
    #else
        #define PINPOINT_C_API
    #endif
#endif

/* =============================================================================
 * Opaque Handle Types
 * ============================================================================= */

/** @brief Opaque handle to a Pinpoint Agent */
typedef struct pinpoint_agent_t* pinpoint_agent_handle;

/** @brief Opaque handle to a Span */
typedef struct pinpoint_span_t* pinpoint_span_handle;

/** @brief Opaque handle to a SpanEvent */
typedef struct pinpoint_span_event_t* pinpoint_span_event_handle;

/** @brief Opaque handle to an Annotation */
typedef struct pinpoint_annotation_t* pinpoint_annotation_handle;

/* =============================================================================
 * Constants
 * ============================================================================= */

/* Annotation Keys */
#define PINPOINT_ANNOTATION_API             12
#define PINPOINT_ANNOTATION_SQL_ID          20
#define PINPOINT_ANNOTATION_SQL_UID         25
#define PINPOINT_ANNOTATION_EXCEPTION_ID    (-52)
#define PINPOINT_ANNOTATION_HTTP_URL        40
#define PINPOINT_ANNOTATION_HTTP_STATUS_CODE 46
#define PINPOINT_ANNOTATION_HTTP_COOKIE     45
#define PINPOINT_ANNOTATION_HTTP_REQUEST_HEADER  47
#define PINPOINT_ANNOTATION_HTTP_RESPONSE_HEADER 55
#define PINPOINT_ANNOTATION_HTTP_PROXY_HEADER    300

/* Application/Service Types */
#define PINPOINT_APP_TYPE_CPP               1300
#define PINPOINT_SERVICE_TYPE_CPP           1300
#define PINPOINT_SERVICE_TYPE_CPP_FUNC      1301
#define PINPOINT_SERVICE_TYPE_CPP_HTTP_CLIENT 9800
#define PINPOINT_SERVICE_TYPE_ASYNC         100

/* Database Service Types */
#define PINPOINT_SERVICE_TYPE_MYSQL_QUERY   2101
#define PINPOINT_SERVICE_TYPE_MSSQL_QUERY   2201
#define PINPOINT_SERVICE_TYPE_ORACLE_QUERY  2301
#define PINPOINT_SERVICE_TYPE_PGSQL_QUERY   2501
#define PINPOINT_SERVICE_TYPE_CASSANDRA_QUERY 2601
#define PINPOINT_SERVICE_TYPE_MONGODB_QUERY 2651

/* Cache/Queue Service Types */
#define PINPOINT_SERVICE_TYPE_MEMCACHED     8050
#define PINPOINT_SERVICE_TYPE_REDIS         8203
#define PINPOINT_SERVICE_TYPE_KAFKA         8660
#define PINPOINT_SERVICE_TYPE_HBASE         8800

/* RPC Service Types */
#define PINPOINT_SERVICE_TYPE_GRPC_CLIENT   9160
#define PINPOINT_SERVICE_TYPE_GRPC_SERVER   1130

/* API Types */
#define PINPOINT_API_TYPE_DEFAULT           0
#define PINPOINT_API_TYPE_WEB_REQUEST       100
#define PINPOINT_API_TYPE_INVOCATION        200

/* Header Types */
typedef enum {
    PINPOINT_HEADER_HTTP_REQUEST = 0,
    PINPOINT_HEADER_HTTP_RESPONSE,
    PINPOINT_HEADER_HTTP_COOKIE
} pinpoint_header_type;

/* =============================================================================
 * Trace Context Callback Types
 * ============================================================================= */

/**
 * @brief Callback function type for reading trace context values.
 * @param key The key to look up.
 * @param value_out Output buffer for the value.
 * @param value_size Size of the output buffer.
 * @param user_data User-provided context pointer.
 * @return Length of the value if found, 0 if not found, -1 on error.
 */
typedef int (*pinpoint_context_reader_fn)(const char* key, char* value_out, 
                                          size_t value_size, void* user_data);

/**
 * @brief Callback function type for writing trace context values.
 * @param key The key to set.
 * @param value The value to set.
 * @param user_data User-provided context pointer.
 */
typedef void (*pinpoint_context_writer_fn)(const char* key, const char* value, 
                                           void* user_data);

/**
 * @brief Callback function type for iterating headers.
 * @param key Header key.
 * @param value Header value.
 * @param user_data User-provided context pointer.
 * @return Non-zero to continue iteration, 0 to stop.
 */
typedef int (*pinpoint_header_iterator_fn)(const char* key, const char* value, 
                                           void* user_data);

/* =============================================================================
 * Configuration Functions
 * ============================================================================= */

/**
 * @brief Set the configuration file path for the agent.
 * @param config_file_path Path to the YAML configuration file.
 */
PINPOINT_C_API void pinpoint_set_config_file_path(const char* config_file_path);

/**
 * @brief Set the configuration string directly.
 * @param config_string YAML configuration string.
 */
PINPOINT_C_API void pinpoint_set_config_string(const char* config_string);

/* =============================================================================
 * Agent Functions
 * ============================================================================= */

/**
 * @brief Create a new Pinpoint agent.
 * @return Handle to the created agent, or NULL on failure.
 */
PINPOINT_C_API pinpoint_agent_handle pinpoint_create_agent(void);

/**
 * @brief Create a new Pinpoint agent with a specific application type.
 * @param app_type The application type identifier.
 * @return Handle to the created agent, or NULL on failure.
 */
PINPOINT_C_API pinpoint_agent_handle pinpoint_create_agent_with_type(int32_t app_type);

/**
 * @brief Get the global singleton agent instance.
 * @return Handle to the global agent, or NULL if not initialized.
 */
PINPOINT_C_API pinpoint_agent_handle pinpoint_global_agent(void);

/**
 * @brief Check if the agent is enabled.
 * @param agent The agent handle.
 * @return true if enabled, false otherwise.
 */
PINPOINT_C_API bool pinpoint_agent_enable(pinpoint_agent_handle agent);

/**
 * @brief Shutdown the agent gracefully.
 * @param agent The agent handle.
 */
PINPOINT_C_API void pinpoint_agent_shutdown(pinpoint_agent_handle agent);

/**
 * @brief Release the agent handle.
 * @param agent The agent handle to release.
 */
PINPOINT_C_API void pinpoint_agent_destroy(pinpoint_agent_handle agent);

/* =============================================================================
 * Span Functions
 * ============================================================================= */

/**
 * @brief Create a new span.
 * @param agent The agent handle.
 * @param operation The operation name.
 * @param rpc_point The RPC endpoint.
 * @return Handle to the created span, or NULL on failure.
 */
PINPOINT_C_API pinpoint_span_handle pinpoint_new_span(
    pinpoint_agent_handle agent,
    const char* operation,
    const char* rpc_point);

/**
 * @brief Create a new span with trace context from inbound carrier.
 * @param agent The agent handle.
 * @param operation The operation name.
 * @param rpc_point The RPC endpoint.
 * @param reader_fn Callback function to read trace context.
 * @param user_data User data passed to the callback.
 * @return Handle to the created span, or NULL on failure.
 */
PINPOINT_C_API pinpoint_span_handle pinpoint_new_span_with_context(
    pinpoint_agent_handle agent,
    const char* operation,
    const char* rpc_point,
    pinpoint_context_reader_fn reader_fn,
    void* user_data);

/**
 * @brief Create a new span with HTTP method and trace context.
 * @param agent The agent handle.
 * @param operation The operation name.
 * @param rpc_point The RPC endpoint.
 * @param method The HTTP method.
 * @param reader_fn Callback function to read trace context.
 * @param user_data User data passed to the callback.
 * @return Handle to the created span, or NULL on failure.
 */
PINPOINT_C_API pinpoint_span_handle pinpoint_new_span_with_method(
    pinpoint_agent_handle agent,
    const char* operation,
    const char* rpc_point,
    const char* method,
    pinpoint_context_reader_fn reader_fn,
    void* user_data);

/**
 * @brief End and finalize the span.
 * @param span The span handle.
 */
PINPOINT_C_API void pinpoint_span_end(pinpoint_span_handle span);

/**
 * @brief Release the span handle.
 * @param span The span handle to release.
 */
PINPOINT_C_API void pinpoint_span_destroy(pinpoint_span_handle span);

/**
 * @brief Set the service type for the span.
 * @param span The span handle.
 * @param service_type The service type identifier.
 */
PINPOINT_C_API void pinpoint_span_set_service_type(pinpoint_span_handle span, 
                                                    int32_t service_type);

/**
 * @brief Set the remote address for the span.
 * @param span The span handle.
 * @param address The remote address.
 */
PINPOINT_C_API void pinpoint_span_set_remote_address(pinpoint_span_handle span, 
                                                      const char* address);

/**
 * @brief Set the endpoint for the span.
 * @param span The span handle.
 * @param end_point The endpoint string.
 */
PINPOINT_C_API void pinpoint_span_set_endpoint(pinpoint_span_handle span, 
                                                const char* end_point);

/**
 * @brief Set an error message on the span.
 * @param span The span handle.
 * @param error_message The error message.
 */
PINPOINT_C_API void pinpoint_span_set_error(pinpoint_span_handle span, 
                                             const char* error_message);

/**
 * @brief Set a named error on the span.
 * @param span The span handle.
 * @param error_name The error name/type.
 * @param error_message The error message.
 */
PINPOINT_C_API void pinpoint_span_set_error_named(pinpoint_span_handle span,
                                                   const char* error_name,
                                                   const char* error_message);

/**
 * @brief Set the HTTP status code for the span.
 * @param span The span handle.
 * @param status The HTTP status code.
 */
PINPOINT_C_API void pinpoint_span_set_status_code(pinpoint_span_handle span, 
                                                   int status);

/**
 * @brief Set URL statistics for the span.
 * @param span The span handle.
 * @param url_pattern The URL pattern.
 * @param method The HTTP method.
 * @param status_code The HTTP status code.
 */
PINPOINT_C_API void pinpoint_span_set_url_stat(pinpoint_span_handle span,
                                                const char* url_pattern,
                                                const char* method,
                                                int status_code);

/**
 * @brief Inject trace context into an outbound carrier.
 * @param span The span handle.
 * @param writer_fn Callback function to write trace context.
 * @param user_data User data passed to the callback.
 */
PINPOINT_C_API void pinpoint_span_inject_context(pinpoint_span_handle span,
                                                  pinpoint_context_writer_fn writer_fn,
                                                  void* user_data);

/**
 * @brief Get the span ID.
 * @param span The span handle.
 * @return The span ID.
 */
PINPOINT_C_API int64_t pinpoint_span_get_span_id(pinpoint_span_handle span);

/**
 * @brief Check if the span is sampled.
 * @param span The span handle.
 * @return true if sampled, false otherwise.
 */
PINPOINT_C_API bool pinpoint_span_is_sampled(pinpoint_span_handle span);

/**
 * @brief Get the trace ID string.
 * @param span The span handle.
 * @param buffer Output buffer for the trace ID.
 * @param buffer_size Size of the output buffer.
 * @return Length of the trace ID string, or -1 on error.
 */
PINPOINT_C_API int pinpoint_span_get_trace_id(pinpoint_span_handle span,
                                               char* buffer,
                                               size_t buffer_size);

/**
 * @brief Get the span annotations.
 * @param span The span handle.
 * @return Handle to the annotations, or NULL on failure.
 */
PINPOINT_C_API pinpoint_annotation_handle pinpoint_span_get_annotations(
    pinpoint_span_handle span);

/**
 * @brief Create an async child span.
 * @param span The parent span handle.
 * @param async_operation The async operation name.
 * @return Handle to the async span, or NULL on failure.
 */
PINPOINT_C_API pinpoint_span_handle pinpoint_span_new_async_span(
    pinpoint_span_handle span,
    const char* async_operation);

/* =============================================================================
 * SpanEvent Functions
 * ============================================================================= */

/**
 * @brief Create a new span event.
 * @param span The span handle.
 * @param operation The operation name.
 * @return Handle to the created span event, or NULL on failure.
 */
PINPOINT_C_API pinpoint_span_event_handle pinpoint_new_span_event(
    pinpoint_span_handle span,
    const char* operation);

/**
 * @brief Create a new span event with service type.
 * @param span The span handle.
 * @param operation The operation name.
 * @param service_type The service type identifier.
 * @return Handle to the created span event, or NULL on failure.
 */
PINPOINT_C_API pinpoint_span_event_handle pinpoint_new_span_event_with_type(
    pinpoint_span_handle span,
    const char* operation,
    int32_t service_type);

/**
 * @brief End the current span event.
 * @param span The span handle.
 */
PINPOINT_C_API void pinpoint_span_event_end(pinpoint_span_handle span);

/**
 * @brief Set the service type for a span event.
 * @param event The span event handle.
 * @param service_type The service type identifier.
 */
PINPOINT_C_API void pinpoint_span_event_set_service_type(
    pinpoint_span_event_handle event,
    int32_t service_type);

/**
 * @brief Set the operation name for a span event.
 * @param event The span event handle.
 * @param operation The operation name.
 */
PINPOINT_C_API void pinpoint_span_event_set_operation_name(
    pinpoint_span_event_handle event,
    const char* operation);

/**
 * @brief Set the destination for a span event.
 * @param event The span event handle.
 * @param destination The destination string.
 */
PINPOINT_C_API void pinpoint_span_event_set_destination(
    pinpoint_span_event_handle event,
    const char* destination);

/**
 * @brief Set the endpoint for a span event.
 * @param event The span event handle.
 * @param end_point The endpoint string.
 */
PINPOINT_C_API void pinpoint_span_event_set_endpoint(
    pinpoint_span_event_handle event,
    const char* end_point);

/**
 * @brief Set an error message on the span event.
 * @param event The span event handle.
 * @param error_message The error message.
 */
PINPOINT_C_API void pinpoint_span_event_set_error(
    pinpoint_span_event_handle event,
    const char* error_message);

/**
 * @brief Set a named error on the span event.
 * @param event The span event handle.
 * @param error_name The error name/type.
 * @param error_message The error message.
 */
PINPOINT_C_API void pinpoint_span_event_set_error_named(
    pinpoint_span_event_handle event,
    const char* error_name,
    const char* error_message);

/**
 * @brief Set SQL query and arguments on the span event.
 * @param event The span event handle.
 * @param sql_query The SQL query string.
 * @param args The query arguments.
 */
PINPOINT_C_API void pinpoint_span_event_set_sql_query(
    pinpoint_span_event_handle event,
    const char* sql_query,
    const char* args);

/**
 * @brief Get the span event annotations.
 * @param event The span event handle.
 * @return Handle to the annotations, or NULL on failure.
 */
PINPOINT_C_API pinpoint_annotation_handle pinpoint_span_event_get_annotations(
    pinpoint_span_event_handle event);

/* =============================================================================
 * Annotation Functions
 * ============================================================================= */

/**
 * @brief Append an integer annotation.
 * @param annotation The annotation handle.
 * @param key The annotation key.
 * @param value The integer value.
 */
PINPOINT_C_API void pinpoint_annotation_append_int(
    pinpoint_annotation_handle annotation,
    int32_t key,
    int32_t value);

/**
 * @brief Append a long annotation.
 * @param annotation The annotation handle.
 * @param key The annotation key.
 * @param value The long value.
 */
PINPOINT_C_API void pinpoint_annotation_append_long(
    pinpoint_annotation_handle annotation,
    int32_t key,
    int64_t value);

/**
 * @brief Append a string annotation.
 * @param annotation The annotation handle.
 * @param key The annotation key.
 * @param value The string value.
 */
PINPOINT_C_API void pinpoint_annotation_append_string(
    pinpoint_annotation_handle annotation,
    int32_t key,
    const char* value);

/**
 * @brief Append a two-string annotation.
 * @param annotation The annotation handle.
 * @param key The annotation key.
 * @param value1 The first string value.
 * @param value2 The second string value.
 */
PINPOINT_C_API void pinpoint_annotation_append_string_string(
    pinpoint_annotation_handle annotation,
    int32_t key,
    const char* value1,
    const char* value2);

/**
 * @brief Append an int-string-string annotation.
 * @param annotation The annotation handle.
 * @param key The annotation key.
 * @param int_value The integer value.
 * @param str_value1 The first string value.
 * @param str_value2 The second string value.
 */
PINPOINT_C_API void pinpoint_annotation_append_int_string_string(
    pinpoint_annotation_handle annotation,
    int32_t key,
    int int_value,
    const char* str_value1,
    const char* str_value2);

/**
 * @brief Release an annotation handle.
 * @param annotation The annotation handle to release.
 */
PINPOINT_C_API void pinpoint_annotation_destroy(pinpoint_annotation_handle annotation);

/* =============================================================================
 * Header Constants (for context propagation)
 * ============================================================================= */

/** @brief HTTP header name for trace ID */
#define PINPOINT_HEADER_TRACE_ID          "Pinpoint-TraceID"
/** @brief HTTP header name for span ID */
#define PINPOINT_HEADER_SPAN_ID           "Pinpoint-SpanID"
/** @brief HTTP header name for parent span ID */
#define PINPOINT_HEADER_PARENT_SPAN_ID    "Pinpoint-pSpanID"
/** @brief HTTP header name for sampled flag */
#define PINPOINT_HEADER_SAMPLED           "Pinpoint-Sampled"
/** @brief HTTP header name for flags */
#define PINPOINT_HEADER_FLAG              "Pinpoint-Flags"
/** @brief HTTP header name for parent application name */
#define PINPOINT_HEADER_PARENT_APP_NAME   "Pinpoint-pAppName"
/** @brief HTTP header name for parent application type */
#define PINPOINT_HEADER_PARENT_APP_TYPE   "Pinpoint-pAppType"
/** @brief HTTP header name for parent application namespace */
#define PINPOINT_HEADER_PARENT_APP_NAMESPACE "Pinpoint-pAppNamespace"
/** @brief HTTP header name for host */
#define PINPOINT_HEADER_HOST              "Pinpoint-Host"

#ifdef __cplusplus
}
#endif

#endif /* PINPOINT_C_H */

