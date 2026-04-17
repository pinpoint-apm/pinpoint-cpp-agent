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
 * @file tracer_c.cpp
 * @brief C++ implementation of the pure-C public API declared in tracer_c.h.
 *
 * Each C handle type wraps a C++ shared_ptr so that object lifetimes are
 * managed safely even when the C caller mixes pt_span_destroy() with
 * pt_span_new_async_span() or pt_span_get_event().
 *
 * Adapter classes (CContextReader, CHeaderReader, CContextWriter,
 * CCallstackReader) bridge the C callback structs to the corresponding
 * pure-virtual C++ interfaces without heap allocation — they are
 * constructed on the stack at each call site.
 */

#include "pinpoint/tracer_c.h"
#include "pinpoint/tracer.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// ============================================================================
// Opaque handle definitions
// ============================================================================

struct pt_agent_s      { pinpoint::AgentPtr      ptr; };
struct pt_span_s       { pinpoint::SpanPtr        ptr; };
struct pt_span_event_s { pinpoint::SpanEventPtr   ptr; };
struct pt_annotation_s { pinpoint::AnnotationPtr  ptr; };

// ============================================================================
// Trampoline helpers
//
// C++ std::function closures cannot be converted to plain C function pointers.
// We use a small context struct placed on the caller's stack together with a
// file-scope trampoline function to bridge between the two worlds without any
// heap allocation.
// ============================================================================

struct ForEachCtx {
    const std::function<bool(std::string_view, std::string_view)>* cb;
};

static int for_each_trampoline(const char* key, const char* value, void* userdata) {
    const auto* ctx = static_cast<const ForEachCtx*>(userdata);
    // Return 0 to continue, non-zero to stop — mirrors the C callback contract.
    return (*ctx->cb)(key, value) ? 0 : 1;
}

struct CallstackForEachCtx {
    const std::function<void(std::string_view, std::string_view, std::string_view, int)>* cb;
};

static void callstack_foreach_trampoline(const char* mod, const char* fn,
                                         const char* file, int line,
                                         void* userdata) {
    const auto* ctx = static_cast<const CallstackForEachCtx*>(userdata);
    (*ctx->cb)(mod ? mod : "", fn ? fn : "", file ? file : "", line);
}

// ============================================================================
// C++ adapter classes — stack-allocated, zero-heap-overhead bridges
// ============================================================================

/**
 * Wraps pt_context_reader_t as a pinpoint::TraceContextReader.
 */
class CContextReader final : public pinpoint::TraceContextReader {
public:
    explicit CContextReader(const pt_context_reader_t* r) : r_(r) {}

    std::optional<std::string> Get(std::string_view key) const override {
        if (!r_ || !r_->get) return std::nullopt;
        std::string k(key);
        const char* v = r_->get(r_->userdata, k.c_str());
        return v ? std::optional<std::string>(v) : std::nullopt;
    }

private:
    const pt_context_reader_t* r_;
};

/**
 * Wraps pt_header_reader_t as a pinpoint::HeaderReader.
 */
class CHeaderReader final : public pinpoint::HeaderReader {
public:
    explicit CHeaderReader(const pt_header_reader_t* r) : r_(r) {}

    std::optional<std::string> Get(std::string_view key) const override {
        if (!r_ || !r_->get) return std::nullopt;
        std::string k(key);
        const char* v = r_->get(r_->userdata, k.c_str());
        return v ? std::optional<std::string>(v) : std::nullopt;
    }

    void ForEach(std::function<bool(std::string_view, std::string_view)> cb) const override {
        if (!r_ || !r_->for_each) return;
        ForEachCtx ctx{&cb};
        r_->for_each(r_->userdata, for_each_trampoline, &ctx);
    }

private:
    const pt_header_reader_t* r_;
};

/**
 * Wraps pt_context_writer_t as a pinpoint::TraceContextWriter.
 */
class CContextWriter final : public pinpoint::TraceContextWriter {
public:
    explicit CContextWriter(pt_context_writer_t* w) : w_(w) {}

    void Set(std::string_view key, std::string_view value) override {
        if (!w_ || !w_->set) return;
        std::string k(key), v(value);
        w_->set(w_->userdata, k.c_str(), v.c_str());
    }

private:
    pt_context_writer_t* w_;
};

/**
 * Wraps pt_callstack_reader_t as a pinpoint::CallStackReader.
 */
class CCallstackReader final : public pinpoint::CallStackReader {
public:
    explicit CCallstackReader(const pt_callstack_reader_t* r) : r_(r) {}

    void ForEach(std::function<void(std::string_view, std::string_view,
                                    std::string_view, int)> cb) const override {
        if (!r_ || !r_->for_each) return;
        CallstackForEachCtx ctx{&cb};
        r_->for_each(r_->userdata, callstack_foreach_trampoline, &ctx);
    }

private:
    const pt_callstack_reader_t* r_;
};

// ============================================================================
// Utility helpers
// ============================================================================

static inline std::chrono::system_clock::time_point ms_to_time_point(int64_t ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

static inline void fill_trace_id(pt_trace_id_t* out, pinpoint::TraceId& tid) {
    std::strncpy(out->agent_id, tid.AgentId.c_str(), PT_AGENT_ID_MAX - 1);
    out->agent_id[PT_AGENT_ID_MAX - 1] = '\0';
    out->start_time = tid.StartTime;
    out->sequence   = tid.Sequence;
}

// ============================================================================
// Global configuration
// ============================================================================

void pt_set_config_file_path(const char* config_file_path) {
    if (config_file_path) {
        pinpoint::SetConfigFilePath(config_file_path);
    }
}

void pt_set_config_string(const char* config_string) {
    if (config_string) {
        pinpoint::SetConfigString(config_string);
    }
}

// ============================================================================
// Agent lifecycle
// ============================================================================

pt_agent_t pt_create_agent(void) {
    auto ptr = pinpoint::CreateAgent();
    if (!ptr) return nullptr;
    return new pt_agent_s{std::move(ptr)};
}

pt_agent_t pt_create_agent_with_type(int32_t app_type) {
    auto ptr = pinpoint::CreateAgent(app_type);
    if (!ptr) return nullptr;
    return new pt_agent_s{std::move(ptr)};
}

pt_agent_t pt_global_agent(void) {
    auto ptr = pinpoint::GlobalAgent();
    if (!ptr) return nullptr;
    // Wrap in a non-owning sentinel: we allocate a handle but the shared_ptr
    // keeps the global agent alive regardless of whether the caller ever calls
    // pt_agent_destroy() on this handle.
    return new pt_agent_s{std::move(ptr)};
}

void pt_agent_destroy(pt_agent_t agent) {
    delete agent;
}

int pt_agent_is_enabled(pt_agent_t agent) {
    if (!agent || !agent->ptr) return 0;
    return agent->ptr->Enable() ? 1 : 0;
}

void pt_agent_shutdown(pt_agent_t agent) {
    if (agent && agent->ptr) {
        agent->ptr->Shutdown();
    }
}

// ============================================================================
// Span creation
// ============================================================================

pt_span_t pt_agent_new_span(pt_agent_t agent, const char* operation,
                            const char* rpc_point) {
    if (!agent || !agent->ptr) return nullptr;
    auto ptr = agent->ptr->NewSpan(operation ? operation : "",
                                   rpc_point  ? rpc_point  : "");
    if (!ptr) return nullptr;
    return new pt_span_s{std::move(ptr)};
}

pt_span_t pt_agent_new_span_with_reader(pt_agent_t agent, const char* operation,
                                        const char* rpc_point,
                                        const pt_context_reader_t* reader) {
    if (!agent || !agent->ptr) return nullptr;
    pinpoint::SpanPtr ptr;
    if (reader) {
        CContextReader cpt_reader(reader);
        ptr = agent->ptr->NewSpan(operation ? operation : "",
                                  rpc_point  ? rpc_point  : "",
                                  cpt_reader);
    } else {
        ptr = agent->ptr->NewSpan(operation ? operation : "",
                                  rpc_point  ? rpc_point  : "");
    }
    if (!ptr) return nullptr;
    return new pt_span_s{std::move(ptr)};
}

pt_span_t pt_agent_new_span_with_method(pt_agent_t agent, const char* operation,
                                        const char* rpc_point, const char* method,
                                        const pt_context_reader_t* reader) {
    if (!agent || !agent->ptr) return nullptr;
    pinpoint::SpanPtr ptr;
    if (reader) {
        CContextReader cpt_reader(reader);
        ptr = agent->ptr->NewSpan(operation ? operation : "",
                                  rpc_point  ? rpc_point  : "",
                                  method     ? method     : "",
                                  cpt_reader);
    } else {
        ptr = agent->ptr->NewSpan(operation ? operation : "",
                                  rpc_point  ? rpc_point  : "");
    }
    if (!ptr) return nullptr;
    return new pt_span_s{std::move(ptr)};
}

// ============================================================================
// Span operations
// ============================================================================

void pt_span_destroy(pt_span_t span) {
    delete span;
}

pt_span_event_t pt_span_new_event(pt_span_t span, const char* operation) {
    if (!span || !span->ptr) return nullptr;
    auto ptr = span->ptr->NewSpanEvent(operation ? operation : "");
    if (!ptr) return nullptr;
    return new pt_span_event_s{std::move(ptr)};
}

pt_span_event_t pt_span_new_event_with_type(pt_span_t span, const char* operation,
                                            int32_t service_type) {
    if (!span || !span->ptr) return nullptr;
    auto ptr = span->ptr->NewSpanEvent(operation ? operation : "", service_type);
    if (!ptr) return nullptr;
    return new pt_span_event_s{std::move(ptr)};
}

pt_span_event_t pt_span_get_event(pt_span_t span) {
    if (!span || !span->ptr) return nullptr;
    auto ptr = span->ptr->GetSpanEvent();
    if (!ptr) return nullptr;
    return new pt_span_event_s{std::move(ptr)};
}

void pt_span_end_event(pt_span_t span) {
    if (span && span->ptr) {
        span->ptr->EndSpanEvent();
    }
}

void pt_span_end(pt_span_t span) {
    if (span && span->ptr) {
        span->ptr->EndSpan();
    }
}

pt_span_t pt_span_new_async_span(pt_span_t span, const char* async_operation) {
    if (!span || !span->ptr) return nullptr;
    auto ptr = span->ptr->NewAsyncSpan(async_operation ? async_operation : "");
    if (!ptr) return nullptr;
    return new pt_span_s{std::move(ptr)};
}

void pt_span_inject_context(pt_span_t span, pt_context_writer_t* writer) {
    if (!span || !span->ptr || !writer) return;
    CContextWriter cpt_writer(writer);
    span->ptr->InjectContext(cpt_writer);
}

void pt_span_extract_context(pt_span_t span, const pt_context_reader_t* reader) {
    if (!span || !span->ptr || !reader) return;
    CContextReader cpt_reader(reader);
    span->ptr->ExtractContext(cpt_reader);
}

pt_trace_id_t pt_span_get_trace_id(pt_span_t span) {
    pt_trace_id_t result{};
    if (span && span->ptr) {
        fill_trace_id(&result, span->ptr->GetTraceId());
    }
    return result;
}

int64_t pt_span_get_span_id(pt_span_t span) {
    if (!span || !span->ptr) return 0;
    return span->ptr->GetSpanId();
}

int pt_span_is_sampled(pt_span_t span) {
    if (!span || !span->ptr) return 0;
    return span->ptr->IsSampled() ? 1 : 0;
}

void pt_span_set_service_type(pt_span_t span, int32_t service_type) {
    if (span && span->ptr) span->ptr->SetServiceType(service_type);
}

void pt_span_set_start_time_ms(pt_span_t span, int64_t ms_since_epoch) {
    if (span && span->ptr) {
        span->ptr->SetStartTime(ms_to_time_point(ms_since_epoch));
    }
}

void pt_span_set_remote_address(pt_span_t span, const char* address) {
    if (span && span->ptr && address) span->ptr->SetRemoteAddress(address);
}

void pt_span_set_end_point(pt_span_t span, const char* end_point) {
    if (span && span->ptr && end_point) span->ptr->SetEndPoint(end_point);
}

void pt_span_set_error(pt_span_t span, const char* error_message) {
    if (span && span->ptr && error_message) span->ptr->SetError(error_message);
}

void pt_span_set_error_named(pt_span_t span, const char* error_name,
                             const char* error_message) {
    if (span && span->ptr && error_name && error_message) {
        span->ptr->SetError(error_name, error_message);
    }
}

void pt_span_set_status_code(pt_span_t span, int status_code) {
    if (span && span->ptr) span->ptr->SetStatusCode(status_code);
}

void pt_span_set_url_stat(pt_span_t span, const char* url_pattern,
                          const char* method, int status_code) {
    if (span && span->ptr) {
        span->ptr->SetUrlStat(url_pattern ? url_pattern : "",
                              method      ? method      : "",
                              status_code);
    }
}

void pt_span_set_logging(pt_span_t span, pt_context_writer_t* writer) {
    if (!span || !span->ptr || !writer) return;
    CContextWriter cpt_writer(writer);
    span->ptr->SetLogging(cpt_writer);
}

void pt_span_record_header(pt_span_t span, pt_header_type_t which,
                           const pt_header_reader_t* reader) {
    if (!span || !span->ptr || !reader) return;
    CHeaderReader cpt_reader(reader);
    span->ptr->RecordHeader(static_cast<pinpoint::HeaderType>(which), cpt_reader);
}

pt_annotation_t pt_span_get_annotations(pt_span_t span) {
    if (!span || !span->ptr) return nullptr;
    auto ptr = span->ptr->GetAnnotations();
    if (!ptr) return nullptr;
    return new pt_annotation_s{std::move(ptr)};
}

// ============================================================================
// SpanEvent operations
// ============================================================================

void pt_span_event_destroy(pt_span_event_t se) {
    delete se;
}

void pt_span_event_set_service_type(pt_span_event_t se, int32_t service_type) {
    if (se && se->ptr) se->ptr->SetServiceType(service_type);
}

void pt_span_event_set_operation_name(pt_span_event_t se, const char* operation) {
    if (se && se->ptr && operation) se->ptr->SetOperationName(operation);
}

void pt_span_event_set_start_time_ms(pt_span_event_t se, int64_t ms_since_epoch) {
    if (se && se->ptr) {
        se->ptr->SetStartTime(ms_to_time_point(ms_since_epoch));
    }
}

void pt_span_event_set_destination(pt_span_event_t se, const char* dest) {
    if (se && se->ptr && dest) se->ptr->SetDestination(dest);
}

void pt_span_event_set_end_point(pt_span_event_t se, const char* end_point) {
    if (se && se->ptr && end_point) se->ptr->SetEndPoint(end_point);
}

void pt_span_event_set_error(pt_span_event_t se, const char* error_message) {
    if (se && se->ptr && error_message) se->ptr->SetError(error_message);
}

void pt_span_event_set_error_named(pt_span_event_t se, const char* error_name,
                                   const char* error_message) {
    if (se && se->ptr && error_name && error_message) {
        se->ptr->SetError(error_name, error_message);
    }
}

void pt_span_event_set_error_with_callstack(pt_span_event_t se,
                                            const char* error_name,
                                            const char* error_message,
                                            const pt_callstack_reader_t* reader) {
    if (!se || !se->ptr) return;
    const char* name = error_name    ? error_name    : "";
    const char* msg  = error_message ? error_message : "";
    if (reader) {
        CCallstackReader cpt_reader(reader);
        se->ptr->SetError(name, msg, cpt_reader);
    } else {
        se->ptr->SetError(name, msg);
    }
}

void pt_span_event_set_sql_query(pt_span_event_t se, const char* sql_query,
                                 const char* args) {
    if (se && se->ptr) {
        se->ptr->SetSqlQuery(sql_query ? sql_query : "",
                             args      ? args      : "");
    }
}

void pt_span_event_record_header(pt_span_event_t se, pt_header_type_t which,
                                 const pt_header_reader_t* reader) {
    if (!se || !se->ptr || !reader) return;
    CHeaderReader cpt_reader(reader);
    se->ptr->RecordHeader(static_cast<pinpoint::HeaderType>(which), cpt_reader);
}

pt_annotation_t pt_span_event_get_annotations(pt_span_event_t se) {
    if (!se || !se->ptr) return nullptr;
    auto ptr = se->ptr->GetAnnotations();
    if (!ptr) return nullptr;
    return new pt_annotation_s{std::move(ptr)};
}

// ============================================================================
// Annotation operations
// ============================================================================

void pt_annotation_destroy(pt_annotation_t anno) {
    delete anno;
}

void pt_annotation_append_int(pt_annotation_t anno, int32_t key, int32_t value) {
    if (anno && anno->ptr) anno->ptr->AppendInt(key, value);
}

void pt_annotation_append_long(pt_annotation_t anno, int32_t key, int64_t value) {
    if (anno && anno->ptr) anno->ptr->AppendLong(key, value);
}

void pt_annotation_append_string(pt_annotation_t anno, int32_t key, const char* value) {
    if (anno && anno->ptr && value) anno->ptr->AppendString(key, value);
}

void pt_annotation_append_string_string(pt_annotation_t anno, int32_t key,
                                        const char* s1, const char* s2) {
    if (anno && anno->ptr) {
        anno->ptr->AppendStringString(key, s1 ? s1 : "", s2 ? s2 : "");
    }
}

void pt_annotation_append_int_string_string(pt_annotation_t anno, int32_t key,
                                            int i, const char* s1, const char* s2) {
    if (anno && anno->ptr) {
        anno->ptr->AppendIntStringString(key, i, s1 ? s1 : "", s2 ? s2 : "");
    }
}

void pt_annotation_append_bytes_string_string(pt_annotation_t anno, int32_t key,
                                              const unsigned char* uid, int uid_len,
                                              const char* s1, const char* s2) {
    if (!anno || !anno->ptr || !uid || uid_len <= 0) return;
    std::vector<unsigned char> v(uid, uid + uid_len);
    anno->ptr->AppendBytesStringString(key, std::move(v),
                                       s1 ? s1 : "", s2 ? s2 : "");
}

void pt_annotation_append_long_int_int_byte_byte_string(pt_annotation_t anno,
                                                        int32_t key,
                                                        int64_t l,
                                                        int32_t i1, int32_t i2,
                                                        int32_t b1, int32_t b2,
                                                        const char* s) {
    if (anno && anno->ptr) {
        anno->ptr->AppendLongIntIntByteByteString(key, l, i1, i2, b1, b2,
                                                  s ? s : "");
    }
}

// ============================================================================
// HTTP helper functions
// ============================================================================

void pt_trace_http_server_request(pt_span_t span,
                                  const char* remote_addr,
                                  const char* endpoint,
                                  const pt_header_reader_t* request_reader) {
    if (!span || !span->ptr || !request_reader) return;
    CHeaderReader cpt_req(request_reader);
    pinpoint::helper::TraceHttpServerRequest(span->ptr,
                                             remote_addr ? remote_addr : "",
                                             endpoint    ? endpoint    : "",
                                             cpt_req);
}

void pt_trace_http_server_request_with_cookie(pt_span_t span,
                                              const char* remote_addr,
                                              const char* endpoint,
                                              const pt_header_reader_t* request_reader,
                                              const pt_header_reader_t* cookie_reader) {
    if (!span || !span->ptr || !request_reader || !cookie_reader) return;
    CHeaderReader cpt_req(request_reader);
    CHeaderReader cpt_cookie(cookie_reader);
    pinpoint::helper::TraceHttpServerRequest(span->ptr,
                                             remote_addr ? remote_addr : "",
                                             endpoint    ? endpoint    : "",
                                             cpt_req, cpt_cookie);
}

void pt_trace_http_server_response(pt_span_t span,
                                   const char* url_pattern,
                                   const char* method,
                                   int status_code,
                                   const pt_header_reader_t* response_reader) {
    if (!span || !span->ptr || !response_reader) return;
    CHeaderReader cpt_resp(response_reader);
    pinpoint::helper::TraceHttpServerResponse(span->ptr,
                                              url_pattern ? url_pattern : "",
                                              method      ? method      : "",
                                              status_code,
                                              cpt_resp);
}

void pt_trace_http_client_request(pt_span_event_t se,
                                  const char* host,
                                  const char* url,
                                  const pt_header_reader_t* request_reader) {
    if (!se || !se->ptr || !request_reader) return;
    CHeaderReader cpt_req(request_reader);
    pinpoint::helper::TraceHttpClientRequest(se->ptr,
                                             host ? host : "",
                                             url  ? url  : "",
                                             cpt_req);
}

void pt_trace_http_client_request_with_cookie(pt_span_event_t se,
                                              const char* host,
                                              const char* url,
                                              const pt_header_reader_t* request_reader,
                                              const pt_header_reader_t* cookie_reader) {
    if (!se || !se->ptr || !request_reader || !cookie_reader) return;
    CHeaderReader cpt_req(request_reader);
    CHeaderReader cpt_cookie(cookie_reader);
    pinpoint::helper::TraceHttpClientRequest(se->ptr,
                                             host ? host : "",
                                             url  ? url  : "",
                                             cpt_req, cpt_cookie);
}

void pt_trace_http_client_response(pt_span_event_t se,
                                   int status_code,
                                   const pt_header_reader_t* response_reader) {
    if (!se || !se->ptr || !response_reader) return;
    CHeaderReader cpt_resp(response_reader);
    pinpoint::helper::TraceHttpClientResponse(se->ptr, status_code, cpt_resp);
}
