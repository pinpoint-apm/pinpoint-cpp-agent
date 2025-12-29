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

#define PINPOINT_C_EXPORTS
#include "pinpoint_c.h"
#include "pinpoint/tracer.h"

#include <cstring>
#include <string>
#include <memory>

/* =============================================================================
 * Internal Handle Structures
 * ============================================================================= */

struct pinpoint_agent_t {
    pinpoint::AgentPtr ptr;
    
    explicit pinpoint_agent_t(pinpoint::AgentPtr p) : ptr(std::move(p)) {}
};

struct pinpoint_span_t {
    pinpoint::SpanPtr ptr;
    
    explicit pinpoint_span_t(pinpoint::SpanPtr p) : ptr(std::move(p)) {}
};

struct pinpoint_span_event_t {
    pinpoint::SpanEventPtr ptr;
    
    explicit pinpoint_span_event_t(pinpoint::SpanEventPtr p) : ptr(std::move(p)) {}
};

struct pinpoint_annotation_t {
    pinpoint::AnnotationPtr ptr;
    
    explicit pinpoint_annotation_t(pinpoint::AnnotationPtr p) : ptr(std::move(p)) {}
};

/* =============================================================================
 * Callback-based Trace Context Adapters
 * ============================================================================= */

class CTraceContextReader : public pinpoint::TraceContextReader {
public:
    CTraceContextReader(pinpoint_context_reader_fn fn, void* user_data)
        : reader_fn_(fn), user_data_(user_data) {}
    
    std::optional<std::string> Get(std::string_view key) const override {
        if (!reader_fn_) {
            return std::nullopt;
        }
        
        // Create null-terminated key string
        std::string key_str(key);
        
        // First call to get the length
        char buffer[1024];
        int len = reader_fn_(key_str.c_str(), buffer, sizeof(buffer), user_data_);
        
        if (len <= 0) {
            return std::nullopt;
        }
        
        return std::string(buffer, static_cast<size_t>(len));
    }
    
private:
    pinpoint_context_reader_fn reader_fn_;
    void* user_data_;
};

class CTraceContextWriter : public pinpoint::TraceContextWriter {
public:
    CTraceContextWriter(pinpoint_context_writer_fn fn, void* user_data)
        : writer_fn_(fn), user_data_(user_data) {}
    
    void Set(std::string_view key, std::string_view value) override {
        if (!writer_fn_) {
            return;
        }
        
        // Create null-terminated strings
        std::string key_str(key);
        std::string value_str(value);
        
        writer_fn_(key_str.c_str(), value_str.c_str(), user_data_);
    }
    
private:
    pinpoint_context_writer_fn writer_fn_;
    void* user_data_;
};

/* =============================================================================
 * Header Reader Adapter for C API
 * 
 * Design: This adapter allows C applications to provide headers to the helper
 * functions using a callback-based pattern:
 * 
 * 1. C application implements pinpoint_header_iterator_fn
 * 2. When iteration is needed, we call the function with this pointer as user_data
 * 3. The C application iterates its headers and calls pinpoint_header_iterator_callback
 * 4. We forward each header to the C++ callback stored in the class member
 * 
 * Implementation uses class member to store callback - thread-safe and clean.
 * ============================================================================= */

class CHeaderReader : public pinpoint::HeaderReader {
public:
    CHeaderReader(pinpoint_header_iterator_fn fn, void* user_data)
        : iterator_fn_(fn), user_data_(user_data), cpp_callback_(nullptr) {}
    
    std::optional<std::string> Get(std::string_view key) const override {
        // Get is not efficiently supported with the callback-based interface
        // The HTTP trace helper functions use ForEach exclusively
        return std::nullopt;
    }
    
    void ForEach(std::function<bool(std::string_view key, std::string_view val)> callback) const override {
        if (!iterator_fn_) {
            return;
        }
        
        cpp_callback_ = &callback;
        iterator_fn_(user_data_, (void*)this);
    }
    
    // Public method to invoke the C++ callback from C code
    void InvokeCallback(const char* key, const char* value) const {
        if (cpp_callback_ && key && value) {
            (*cpp_callback_)(key, value);
        }
    }
    
private:
    pinpoint_header_iterator_fn iterator_fn_;
    void* user_data_;
    mutable const std::function<bool(std::string_view, std::string_view)>* cpp_callback_;
};

extern "C" {

// Public callback function that C applications call for each header during iteration
PINPOINT_C_API void pinpoint_header_iterator_callback(const char* key, const char* value, void* reader_context) {
    if (!reader_context) {
        return;
    }
    
    auto* reader = static_cast<CHeaderReader*>(reader_context);
    if (reader) {
        reader->InvokeCallback(key, value);
    }
}

} // extern "C"

/* =============================================================================
 * Configuration Functions
 * ============================================================================= */

extern "C" {

PINPOINT_C_API void pinpoint_set_config_file_path(const char* config_file_path) {
    if (config_file_path) {
        pinpoint::SetConfigFilePath(config_file_path);
    }
}

PINPOINT_C_API void pinpoint_set_config_string(const char* config_string) {
    if (config_string) {
        pinpoint::SetConfigString(config_string);
    }
}

/* =============================================================================
 * Agent Functions
 * ============================================================================= */

PINPOINT_C_API pinpoint_agent_handle pinpoint_create_agent(void) {
    try {
        auto agent = pinpoint::CreateAgent();
        if (agent) {
            return new pinpoint_agent_t(std::move(agent));
        }
    } catch (...) {
        // Silently handle exceptions
    }
    return nullptr;
}

PINPOINT_C_API pinpoint_agent_handle pinpoint_create_agent_with_type(int32_t app_type) {
    try {
        auto agent = pinpoint::CreateAgent(app_type);
        if (agent) {
            return new pinpoint_agent_t(std::move(agent));
        }
    } catch (...) {
        // Silently handle exceptions
    }
    return nullptr;
}

PINPOINT_C_API pinpoint_agent_handle pinpoint_global_agent(void) {
    try {
        auto agent = pinpoint::GlobalAgent();
        if (agent) {
            return new pinpoint_agent_t(agent);
        }
    } catch (...) {
        // Silently handle exceptions
    }
    return nullptr;
}

PINPOINT_C_API bool pinpoint_agent_enable(pinpoint_agent_handle agent) {
    if (agent && agent->ptr) {
        return agent->ptr->Enable();
    }
    return false;
}

PINPOINT_C_API void pinpoint_agent_shutdown(pinpoint_agent_handle agent) {
    if (agent && agent->ptr) {
        agent->ptr->Shutdown();
    }
}

PINPOINT_C_API void pinpoint_agent_destroy(pinpoint_agent_handle agent) {
    delete agent;
}

/* =============================================================================
 * Span Functions
 * ============================================================================= */

PINPOINT_C_API pinpoint_span_handle pinpoint_new_span(
    pinpoint_agent_handle agent,
    const char* operation,
    const char* rpc_point) {
    
    if (!agent || !agent->ptr || !operation || !rpc_point) {
        return nullptr;
    }
    
    try {
        auto span = agent->ptr->NewSpan(operation, rpc_point);
        if (span) {
            return new pinpoint_span_t(std::move(span));
        }
    } catch (...) {
        // Silently handle exceptions
    }
    return nullptr;
}

PINPOINT_C_API pinpoint_span_handle pinpoint_new_span_with_context(
    pinpoint_agent_handle agent,
    const char* operation,
    const char* rpc_point,
    pinpoint_context_reader_fn reader_fn,
    void* user_data) {
    
    if (!agent || !agent->ptr || !operation || !rpc_point) {
        return nullptr;
    }
    
    try {
        CTraceContextReader reader(reader_fn, user_data);
        auto span = agent->ptr->NewSpan(operation, rpc_point, reader);
        if (span) {
            return new pinpoint_span_t(std::move(span));
        }
    } catch (...) {
        // Silently handle exceptions
    }
    return nullptr;
}

PINPOINT_C_API pinpoint_span_handle pinpoint_new_span_with_method(
    pinpoint_agent_handle agent,
    const char* operation,
    const char* rpc_point,
    const char* method,
    pinpoint_context_reader_fn reader_fn,
    void* user_data) {
    
    if (!agent || !agent->ptr || !operation || !rpc_point || !method) {
        return nullptr;
    }
    
    try {
        CTraceContextReader reader(reader_fn, user_data);
        auto span = agent->ptr->NewSpan(operation, rpc_point, method, reader);
        if (span) {
            return new pinpoint_span_t(std::move(span));
        }
    } catch (...) {
        // Silently handle exceptions
    }
    return nullptr;
}

PINPOINT_C_API void pinpoint_span_end(pinpoint_span_handle span) {
    if (span && span->ptr) {
        span->ptr->EndSpan();
    }
}

PINPOINT_C_API void pinpoint_span_destroy(pinpoint_span_handle span) {
    delete span;
}

PINPOINT_C_API void pinpoint_span_set_service_type(pinpoint_span_handle span, 
                                                    int32_t service_type) {
    if (span && span->ptr) {
        span->ptr->SetServiceType(service_type);
    }
}

PINPOINT_C_API void pinpoint_span_set_remote_address(pinpoint_span_handle span, 
                                                      const char* address) {
    if (span && span->ptr && address) {
        span->ptr->SetRemoteAddress(address);
    }
}

PINPOINT_C_API void pinpoint_span_set_endpoint(pinpoint_span_handle span, 
                                                const char* end_point) {
    if (span && span->ptr && end_point) {
        span->ptr->SetEndPoint(end_point);
    }
}

PINPOINT_C_API void pinpoint_span_set_error(pinpoint_span_handle span, 
                                             const char* error_message) {
    if (span && span->ptr && error_message) {
        span->ptr->SetError(error_message);
    }
}

PINPOINT_C_API void pinpoint_span_set_error_named(pinpoint_span_handle span,
                                                   const char* error_name,
                                                   const char* error_message) {
    if (span && span->ptr && error_name && error_message) {
        span->ptr->SetError(error_name, error_message);
    }
}

PINPOINT_C_API void pinpoint_span_set_status_code(pinpoint_span_handle span, 
                                                   int status) {
    if (span && span->ptr) {
        span->ptr->SetStatusCode(status);
    }
}

PINPOINT_C_API void pinpoint_span_set_url_stat(pinpoint_span_handle span,
                                                const char* url_pattern,
                                                const char* method,
                                                int status_code) {
    if (span && span->ptr && url_pattern && method) {
        span->ptr->SetUrlStat(url_pattern, method, status_code);
    }
}

PINPOINT_C_API void pinpoint_span_inject_context(pinpoint_span_handle span,
                                                  pinpoint_context_writer_fn writer_fn,
                                                  void* user_data) {
    if (span && span->ptr && writer_fn) {
        CTraceContextWriter writer(writer_fn, user_data);
        span->ptr->InjectContext(writer);
    }
}

PINPOINT_C_API int64_t pinpoint_span_get_span_id(pinpoint_span_handle span) {
    if (span && span->ptr) {
        return span->ptr->GetSpanId();
    }
    return 0;
}

PINPOINT_C_API bool pinpoint_span_is_sampled(pinpoint_span_handle span) {
    if (span && span->ptr) {
        return span->ptr->IsSampled();
    }
    return false;
}

PINPOINT_C_API int pinpoint_span_get_trace_id(pinpoint_span_handle span,
                                               char* buffer,
                                               size_t buffer_size) {
    if (!span || !span->ptr || !buffer || buffer_size == 0) {
        return -1;
    }
    
    std::string trace_id = span->ptr->GetTraceId().ToString();
    size_t len = trace_id.length();
    
    if (len >= buffer_size) {
        // Buffer too small, copy what we can
        std::memcpy(buffer, trace_id.c_str(), buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
        return static_cast<int>(len);
    }
    
    std::memcpy(buffer, trace_id.c_str(), len + 1);
    return static_cast<int>(len);
}

PINPOINT_C_API pinpoint_annotation_handle pinpoint_span_get_annotations(
    pinpoint_span_handle span) {
    
    if (!span || !span->ptr) {
        return nullptr;
    }
    
    try {
        auto annotations = span->ptr->GetAnnotations();
        if (annotations) {
            return new pinpoint_annotation_t(annotations);
        }
    } catch (...) {
        // Silently handle exceptions
    }
    return nullptr;
}

PINPOINT_C_API pinpoint_span_handle pinpoint_span_new_async_span(
    pinpoint_span_handle span,
    const char* async_operation) {
    
    if (!span || !span->ptr || !async_operation) {
        return nullptr;
    }
    
    try {
        auto async_span = span->ptr->NewAsyncSpan(async_operation);
        if (async_span) {
            return new pinpoint_span_t(std::move(async_span));
        }
    } catch (...) {
        // Silently handle exceptions
    }
    return nullptr;
}

/* =============================================================================
 * SpanEvent Functions
 * ============================================================================= */

PINPOINT_C_API pinpoint_span_event_handle pinpoint_new_span_event(
    pinpoint_span_handle span,
    const char* operation) {
    
    if (!span || !span->ptr || !operation) {
        return nullptr;
    }
    
    try {
        auto event = span->ptr->NewSpanEvent(operation);
        if (event) {
            return new pinpoint_span_event_t(std::move(event));
        }
    } catch (...) {
        // Silently handle exceptions
    }
    return nullptr;
}

PINPOINT_C_API pinpoint_span_event_handle pinpoint_new_span_event_with_type(
    pinpoint_span_handle span,
    const char* operation,
    int32_t service_type) {
    
    if (!span || !span->ptr || !operation) {
        return nullptr;
    }
    
    try {
        auto event = span->ptr->NewSpanEvent(operation, service_type);
        if (event) {
            return new pinpoint_span_event_t(std::move(event));
        }
    } catch (...) {
        // Silently handle exceptions
    }
    return nullptr;
}

PINPOINT_C_API void pinpoint_span_event_end(pinpoint_span_handle span) {
    if (span && span->ptr) {
        span->ptr->EndSpanEvent();
    }
}

PINPOINT_C_API void pinpoint_span_event_set_service_type(
    pinpoint_span_event_handle event,
    int32_t service_type) {
    
    if (event && event->ptr) {
        event->ptr->SetServiceType(service_type);
    }
}

PINPOINT_C_API void pinpoint_span_event_set_operation_name(
    pinpoint_span_event_handle event,
    const char* operation) {
    
    if (event && event->ptr && operation) {
        event->ptr->SetOperationName(operation);
    }
}

PINPOINT_C_API void pinpoint_span_event_set_destination(
    pinpoint_span_event_handle event,
    const char* destination) {
    
    if (event && event->ptr && destination) {
        event->ptr->SetDestination(destination);
    }
}

PINPOINT_C_API void pinpoint_span_event_set_endpoint(
    pinpoint_span_event_handle event,
    const char* end_point) {
    
    if (event && event->ptr && end_point) {
        event->ptr->SetEndPoint(end_point);
    }
}

PINPOINT_C_API void pinpoint_span_event_set_error(
    pinpoint_span_event_handle event,
    const char* error_message) {
    
    if (event && event->ptr && error_message) {
        event->ptr->SetError(error_message);
    }
}

PINPOINT_C_API void pinpoint_span_event_set_error_named(
    pinpoint_span_event_handle event,
    const char* error_name,
    const char* error_message) {
    
    if (event && event->ptr && error_name && error_message) {
        event->ptr->SetError(error_name, error_message);
    }
}

PINPOINT_C_API void pinpoint_span_event_set_sql_query(
    pinpoint_span_event_handle event,
    const char* sql_query,
    const char* args) {
    
    if (event && event->ptr && sql_query) {
        event->ptr->SetSqlQuery(sql_query, args ? args : "");
    }
}

PINPOINT_C_API pinpoint_annotation_handle pinpoint_span_event_get_annotations(
    pinpoint_span_event_handle event) {
    
    if (!event || !event->ptr) {
        return nullptr;
    }
    
    try {
        auto annotations = event->ptr->GetAnnotations();
        if (annotations) {
            return new pinpoint_annotation_t(annotations);
        }
    } catch (...) {
        // Silently handle exceptions
    }
    return nullptr;
}

/* =============================================================================
 * Annotation Functions
 * ============================================================================= */

PINPOINT_C_API void pinpoint_annotation_append_int(
    pinpoint_annotation_handle annotation,
    int32_t key,
    int32_t value) {
    
    if (annotation && annotation->ptr) {
        annotation->ptr->AppendInt(key, value);
    }
}

PINPOINT_C_API void pinpoint_annotation_append_long(
    pinpoint_annotation_handle annotation,
    int32_t key,
    int64_t value) {
    
    if (annotation && annotation->ptr) {
        annotation->ptr->AppendLong(key, value);
    }
}

PINPOINT_C_API void pinpoint_annotation_append_string(
    pinpoint_annotation_handle annotation,
    int32_t key,
    const char* value) {
    
    if (annotation && annotation->ptr && value) {
        annotation->ptr->AppendString(key, value);
    }
}

PINPOINT_C_API void pinpoint_annotation_append_string_string(
    pinpoint_annotation_handle annotation,
    int32_t key,
    const char* value1,
    const char* value2) {
    
    if (annotation && annotation->ptr && value1 && value2) {
        annotation->ptr->AppendStringString(key, value1, value2);
    }
}

PINPOINT_C_API void pinpoint_annotation_append_int_string_string(
    pinpoint_annotation_handle annotation,
    int32_t key,
    int int_value,
    const char* str_value1,
    const char* str_value2) {
    
    if (annotation && annotation->ptr && str_value1 && str_value2) {
        annotation->ptr->AppendIntStringString(key, int_value, str_value1, str_value2);
    }
}

PINPOINT_C_API void pinpoint_annotation_destroy(pinpoint_annotation_handle annotation) {
    delete annotation;
}

/* =============================================================================
 * HTTP Trace Helper Functions
 * ============================================================================= */

PINPOINT_C_API void pinpoint_trace_http_server_request(
    pinpoint_span_handle span,
    const char* remote_addr,
    const char* endpoint,
    pinpoint_header_iterator_fn request_iterator_fn,
    void* request_user_data) {
    
    if (!span || !span->ptr || !remote_addr || !endpoint) {
        return;
    }
    
    try {
        CHeaderReader request_reader(request_iterator_fn, request_user_data);
        pinpoint::helper::TraceHttpServerRequest(span->ptr, remote_addr, endpoint, request_reader);
    } catch (...) {
        // Silently handle exceptions
    }
}

PINPOINT_C_API void pinpoint_trace_http_server_request_with_cookies(
    pinpoint_span_handle span,
    const char* remote_addr,
    const char* endpoint,
    pinpoint_header_iterator_fn request_iterator_fn,
    void* request_user_data,
    pinpoint_header_iterator_fn cookie_iterator_fn,
    void* cookie_user_data) {
    
    if (!span || !span->ptr || !remote_addr || !endpoint) {
        return;
    }
    
    try {
        CHeaderReader request_reader(request_iterator_fn, request_user_data);
        CHeaderReader cookie_reader(cookie_iterator_fn, cookie_user_data);
        pinpoint::helper::TraceHttpServerRequest(span->ptr, remote_addr, endpoint, request_reader, cookie_reader);
    } catch (...) {
        // Silently handle exceptions
    }
}

PINPOINT_C_API void pinpoint_trace_http_server_response(
    pinpoint_span_handle span,
    const char* url_pattern,
    const char* method,
    int status_code,
    pinpoint_header_iterator_fn response_iterator_fn,
    void* response_user_data) {
    
    if (!span || !span->ptr || !url_pattern || !method) {
        return;
    }
    
    try {
        CHeaderReader response_reader(response_iterator_fn, response_user_data);
        pinpoint::helper::TraceHttpServerResponse(span->ptr, url_pattern, method, status_code, response_reader);
    } catch (...) {
        // Silently handle exceptions
    }
}

PINPOINT_C_API void pinpoint_trace_http_client_request(
    pinpoint_span_event_handle span_event,
    const char* host,
    const char* url,
    pinpoint_header_iterator_fn request_iterator_fn,
    void* request_user_data) {
    
    if (!span_event || !span_event->ptr || !host || !url) {
        return;
    }
    
    try {
        CHeaderReader request_reader(request_iterator_fn, request_user_data);
        pinpoint::helper::TraceHttpClientRequest(span_event->ptr, host, url, request_reader);
    } catch (...) {
        // Silently handle exceptions
    }
}

PINPOINT_C_API void pinpoint_trace_http_client_request_with_cookies(
    pinpoint_span_event_handle span_event,
    const char* host,
    const char* url,
    pinpoint_header_iterator_fn request_iterator_fn,
    void* request_user_data,
    pinpoint_header_iterator_fn cookie_iterator_fn,
    void* cookie_user_data) {
    
    if (!span_event || !span_event->ptr || !host || !url) {
        return;
    }
    
    try {
        CHeaderReader request_reader(request_iterator_fn, request_user_data);
        CHeaderReader cookie_reader(cookie_iterator_fn, cookie_user_data);
        pinpoint::helper::TraceHttpClientRequest(span_event->ptr, host, url, request_reader, cookie_reader);
    } catch (...) {
        // Silently handle exceptions
    }
}

PINPOINT_C_API void pinpoint_trace_http_client_response(
    pinpoint_span_event_handle span_event,
    int status_code,
    pinpoint_header_iterator_fn response_iterator_fn,
    void* response_user_data) {
    
    if (!span_event || !span_event->ptr) {
        return;
    }
    
    try {
        CHeaderReader response_reader(response_iterator_fn, response_user_data);
        pinpoint::helper::TraceHttpClientResponse(span_event->ptr, status_code, response_reader);
    } catch (...) {
        // Silently handle exceptions
    }
}

} // extern "C"

