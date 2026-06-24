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

#include "logging.h"
#include "noop.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// ============================================================================
// Exception firewall
//
// An exception unwinding through a C stack frame is undefined behavior
// (typically std::terminate in the host application), so every pt_* entry
// point wraps its body in try/catch and returns a safe default on failure.
// pt_handle_exception() is called from those catch(...) handlers to log the
// active exception; it must itself never throw.
// ============================================================================

static void pt_handle_exception(const char* func) noexcept {
    try {
        using pinpoint::Logger;
        using pinpoint::kFileName;
        try {
            throw;
        } catch (const std::exception& e) {
            LOG_ERROR("{}: exception = {}", func, e.what());
        } catch (...) {
            LOG_ERROR("{}: unknown exception", func);
        }
    } catch (...) {
        // Logging itself failed; swallow — the firewall must hold.
    }
}

// ============================================================================
// Opaque handle definitions
// ============================================================================

struct pt_agent_s      { pinpoint::AgentPtr      ptr; };
struct pt_span_s       { pinpoint::SpanPtr        ptr; };
struct pt_span_event_s { pinpoint::SpanEventPtr   ptr; };
struct pt_annotation_s { pinpoint::AnnotationPtr  ptr; };

// ============================================================================
// Static noop sentinel handles
//
// The C++ layer treats noop work as free: a disabled / filtered / unsampled
// call hands back a shared_ptr to one of a handful of process-wide noop
// singletons (noopAgent / noopSpan / noopSpanEvent / noopAnnotation). The C
// wrapper used to break that contract — every such call still malloc()'d a
// fresh handle and copied the singleton's shared_ptr into it, so a hot path
// doing zero real tracing paid for an allocation, a free, and a pair of atomic
// refcount operations on a single control block shared by every thread (a
// cross-core cacheline contention point).
//
// To restore "noop is free" at the C layer we hand back one static sentinel
// handle per noop type. Each sentinel owns a single long-lived reference to its
// singleton, taken once on first use, so operations routed through
// sentinel->ptr remain correct no-ops while creation skips the allocation and
// destruction skips the free. Sentinels live for the lifetime of the process
// and must never be delete'd.
//
// Lazy function-local statics give thread-safe initialization (C++11) and the
// correct teardown order: each sentinel is constructed after the noop singleton
// it references (noopXxx() is called during the sentinel's own init), so it is
// destroyed first and its reference keeps the singleton alive until then.
// ============================================================================

static pt_agent_s* noop_agent_sentinel() {
    static pt_agent_s sentinel{pinpoint::noopAgent()};
    return &sentinel;
}

static pt_span_s* noop_span_sentinel() {
    static pt_span_s sentinel{pinpoint::noopSpan()};
    return &sentinel;
}

static pt_span_event_s* noop_span_event_sentinel() {
    static pt_span_event_s sentinel{pinpoint::noopSpanEvent()};
    return &sentinel;
}

static pt_annotation_s* noop_annotation_sentinel() {
    static pt_annotation_s sentinel{pinpoint::noopAnnotation()};
    return &sentinel;
}

// Wrap a C++ result in a freshly allocated handle, unless it is the shared noop
// singleton — in which case hand back the static sentinel and let the local
// reference drop. A null result maps to a null handle. The pointer comparison
// is exact: a real (live) object can never share an address with the live noop
// singleton, so this never misclassifies a span that should record.

static pt_agent_t make_agent_handle(pinpoint::AgentPtr ptr) {
    if (!ptr) return nullptr;
    if (ptr.get() == noop_agent_sentinel()->ptr.get()) return noop_agent_sentinel();
    return new pt_agent_s{std::move(ptr)};
}

static pt_span_t make_span_handle(pinpoint::SpanPtr ptr) {
    if (!ptr) return nullptr;
    if (ptr.get() == noop_span_sentinel()->ptr.get()) return noop_span_sentinel();
    return new pt_span_s{std::move(ptr)};
}

static pt_span_event_t make_span_event_handle(pinpoint::SpanEventPtr ptr) {
    if (!ptr) return nullptr;
    if (ptr.get() == noop_span_event_sentinel()->ptr.get()) return noop_span_event_sentinel();
    return new pt_span_event_s{std::move(ptr)};
}

static pt_annotation_t make_annotation_handle(pinpoint::AnnotationPtr ptr) {
    if (!ptr) return nullptr;
    if (ptr.get() == noop_annotation_sentinel()->ptr.get()) return noop_annotation_sentinel();
    return new pt_annotation_s{std::move(ptr)};
}

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

static std::vector<std::string> to_string_vector(const char* const* values, int count) {
    std::vector<std::string> out;
    if (!values || count <= 0) {
        return out;
    }
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (values[i]) {
            out.emplace_back(values[i]);
        }
    }
    return out;
}

// ============================================================================
// Global configuration
// ============================================================================

void pt_set_config_file_path(const char* config_file_path) try {
    if (config_file_path) {
        pinpoint::SetConfigFilePath(config_file_path);
    }
} catch (...) { pt_handle_exception(__func__); }

void pt_set_config_string(const char* config_string) try {
    if (config_string) {
        pinpoint::SetConfigString(config_string);
    }
} catch (...) { pt_handle_exception(__func__); }

// ============================================================================
// Agent lifecycle
// ============================================================================

pt_agent_t pt_create_agent(void) try {
    return make_agent_handle(pinpoint::CreateAgent());
} catch (...) { pt_handle_exception(__func__); return nullptr; }

pt_agent_t pt_create_agent_with_server_metadata(const char* server_info,
                                                const char* const* args,
                                                int args_count,
                                                const char* const* libs,
                                                int libs_count) try {
    return make_agent_handle(server_info
        ? pinpoint::CreateAgent(server_info,
                                to_string_vector(args, args_count),
                                to_string_vector(libs, libs_count))
        : pinpoint::CreateAgent());
} catch (...) { pt_handle_exception(__func__); return nullptr; }

pt_agent_t pt_create_agent_with_type(int32_t app_type) try {
    return make_agent_handle(pinpoint::CreateAgent(app_type));
} catch (...) { pt_handle_exception(__func__); return nullptr; }

pt_agent_t pt_create_agent_with_type_and_server_metadata(int32_t app_type,
                                                         const char* server_info,
                                                         const char* const* args,
                                                         int args_count,
                                                         const char* const* libs,
                                                         int libs_count) try {
    return make_agent_handle(server_info
        ? pinpoint::CreateAgent(app_type,
                                server_info,
                                to_string_vector(args, args_count),
                                to_string_vector(libs, libs_count))
        : pinpoint::CreateAgent(app_type));
} catch (...) { pt_handle_exception(__func__); return nullptr; }

pt_agent_t pt_global_agent(void) try {
    // A missing global agent collapses to the shared noop-agent sentinel; an
    // existing one is wrapped in a handle whose shared_ptr keeps it alive
    // regardless of whether the caller ever calls pt_agent_destroy().
    return make_agent_handle(pinpoint::GlobalAgent());
} catch (...) { pt_handle_exception(__func__); return nullptr; }

void pt_agent_destroy(pt_agent_t agent) try {
    if (agent == noop_agent_sentinel()) return;  // static sentinel — never owned
    delete agent;
} catch (...) { pt_handle_exception(__func__); }

int pt_agent_is_enabled(pt_agent_t agent) try {
    if (!agent || !agent->ptr) return 0;
    return agent->ptr->Enable() ? 1 : 0;
} catch (...) { pt_handle_exception(__func__); return 0; }

void pt_agent_shutdown(pt_agent_t agent) try {
    if (agent && agent->ptr) {
        agent->ptr->Shutdown();
    }
} catch (...) { pt_handle_exception(__func__); }

// ============================================================================
// Span creation
// ============================================================================

pt_span_t pt_agent_new_span(pt_agent_t agent, const char* operation,
                            const char* rpc_point) try {
    if (!agent || !agent->ptr) return nullptr;
    // The noop agent only ever makes noop spans — skip the call (and the
    // singleton refcount churn it would trigger) and hand back the sentinel.
    if (agent == noop_agent_sentinel()) return noop_span_sentinel();
    return make_span_handle(agent->ptr->NewSpan(operation ? operation : "",
                                                rpc_point  ? rpc_point  : ""));
} catch (...) { pt_handle_exception(__func__); return nullptr; }

pt_span_t pt_agent_new_span_with_reader(pt_agent_t agent, const char* operation,
                                        const char* rpc_point,
                                        const pt_context_reader_t* reader) try {
    if (!agent || !agent->ptr) return nullptr;
    if (agent == noop_agent_sentinel()) return noop_span_sentinel();
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
    return make_span_handle(std::move(ptr));
} catch (...) { pt_handle_exception(__func__); return nullptr; }

pt_span_t pt_agent_new_span_with_method(pt_agent_t agent, const char* operation,
                                        const char* rpc_point, const char* method,
                                        const pt_context_reader_t* reader) try {
    if (!agent || !agent->ptr) return nullptr;
    if (agent == noop_agent_sentinel()) return noop_span_sentinel();
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
    return make_span_handle(std::move(ptr));
} catch (...) { pt_handle_exception(__func__); return nullptr; }

// ============================================================================
// Span operations
// ============================================================================

void pt_span_destroy(pt_span_t span) try {
    if (span == noop_span_sentinel()) return;  // static sentinel — never owned
    delete span;
} catch (...) { pt_handle_exception(__func__); }

pt_span_event_t pt_span_new_event(pt_span_t span, const char* operation) try {
    if (!span || !span->ptr) return nullptr;
    if (span == noop_span_sentinel()) return noop_span_event_sentinel();
    return make_span_event_handle(span->ptr->NewSpanEvent(operation ? operation : ""));
} catch (...) { pt_handle_exception(__func__); return nullptr; }

pt_span_event_t pt_span_new_event_with_type(pt_span_t span, const char* operation,
                                            int32_t service_type) try {
    if (!span || !span->ptr) return nullptr;
    if (span == noop_span_sentinel()) return noop_span_event_sentinel();
    return make_span_event_handle(
        span->ptr->NewSpanEvent(operation ? operation : "", service_type));
} catch (...) { pt_handle_exception(__func__); return nullptr; }

pt_span_event_t pt_span_get_event(pt_span_t span) try {
    if (!span || !span->ptr) return nullptr;
    if (span == noop_span_sentinel()) return noop_span_event_sentinel();
    return make_span_event_handle(span->ptr->GetSpanEvent());
} catch (...) { pt_handle_exception(__func__); return nullptr; }

void pt_span_end_event(pt_span_t span) try {
    if (span && span->ptr) {
        span->ptr->EndSpanEvent();
    }
} catch (...) { pt_handle_exception(__func__); }

void pt_span_end(pt_span_t span) try {
    if (span && span->ptr) {
        span->ptr->EndSpan();
    }
} catch (...) { pt_handle_exception(__func__); }

pt_span_t pt_span_new_async_span(pt_span_t span, const char* async_operation) try {
    if (!span || !span->ptr) return nullptr;
    if (span == noop_span_sentinel()) return noop_span_sentinel();
    return make_span_handle(
        span->ptr->NewAsyncSpan(async_operation ? async_operation : ""));
} catch (...) { pt_handle_exception(__func__); return nullptr; }

void pt_span_inject_context(pt_span_t span, pt_context_writer_t* writer) try {
    if (!span || !span->ptr || !writer) return;
    CContextWriter cpt_writer(writer);
    span->ptr->InjectContext(cpt_writer);
} catch (...) { pt_handle_exception(__func__); }

void pt_span_extract_context(pt_span_t span, const pt_context_reader_t* reader) try {
    if (!span || !span->ptr || !reader) return;
    CContextReader cpt_reader(reader);
    span->ptr->ExtractContext(cpt_reader);
} catch (...) { pt_handle_exception(__func__); }

pt_trace_id_t pt_span_get_trace_id(pt_span_t span) try {
    pt_trace_id_t result{};
    if (span && span->ptr) {
        fill_trace_id(&result, span->ptr->GetTraceId());
    }
    return result;
} catch (...) { pt_handle_exception(__func__); return pt_trace_id_t{}; }

int64_t pt_span_get_span_id(pt_span_t span) try {
    if (!span || !span->ptr) return 0;
    return span->ptr->GetSpanId();
} catch (...) { pt_handle_exception(__func__); return 0; }

int pt_span_is_sampled(pt_span_t span) try {
    if (!span || !span->ptr) return 0;
    return span->ptr->IsSampled() ? 1 : 0;
} catch (...) { pt_handle_exception(__func__); return 0; }

void pt_span_set_service_type(pt_span_t span, int32_t service_type) try {
    if (span && span->ptr) span->ptr->SetServiceType(service_type);
} catch (...) { pt_handle_exception(__func__); }

void pt_span_set_start_time_ms(pt_span_t span, int64_t ms_since_epoch) try {
    if (span && span->ptr) {
        span->ptr->SetStartTime(ms_to_time_point(ms_since_epoch));
    }
} catch (...) { pt_handle_exception(__func__); }

void pt_span_set_remote_address(pt_span_t span, const char* address) try {
    if (span && span->ptr && address) span->ptr->SetRemoteAddress(address);
} catch (...) { pt_handle_exception(__func__); }

void pt_span_set_end_point(pt_span_t span, const char* end_point) try {
    if (span && span->ptr && end_point) span->ptr->SetEndPoint(end_point);
} catch (...) { pt_handle_exception(__func__); }

void pt_span_set_acceptor_host(pt_span_t span, const char* host) try {
    if (span && span->ptr && host) span->ptr->SetAcceptorHost(host);
} catch (...) { pt_handle_exception(__func__); }

void pt_span_set_error(pt_span_t span, const char* error_message) try {
    if (span && span->ptr && error_message) span->ptr->SetError(error_message);
} catch (...) { pt_handle_exception(__func__); }

void pt_span_set_error_named(pt_span_t span, const char* error_name,
                             const char* error_message) try {
    if (span && span->ptr && error_name && error_message) {
        span->ptr->SetError(error_name, error_message);
    }
} catch (...) { pt_handle_exception(__func__); }

void pt_span_set_status_code(pt_span_t span, int status_code) try {
    if (span && span->ptr) span->ptr->SetStatusCode(status_code);
} catch (...) { pt_handle_exception(__func__); }

void pt_span_set_url_stat(pt_span_t span, const char* url_pattern,
                          const char* method, int status_code) try {
    if (span && span->ptr) {
        span->ptr->SetUrlStat(url_pattern ? url_pattern : "",
                              method      ? method      : "",
                              status_code);
    }
} catch (...) { pt_handle_exception(__func__); }

void pt_span_set_logging(pt_span_t span, pt_context_writer_t* writer) try {
    if (!span || !span->ptr || !writer) return;
    CContextWriter cpt_writer(writer);
    span->ptr->SetLogging(cpt_writer);
} catch (...) { pt_handle_exception(__func__); }

void pt_span_record_header(pt_span_t span, pt_header_type_t which,
                           const pt_header_reader_t* reader) try {
    if (!span || !span->ptr || !reader) return;
    CHeaderReader cpt_reader(reader);
    span->ptr->RecordHeader(static_cast<pinpoint::HeaderType>(which), cpt_reader);
} catch (...) { pt_handle_exception(__func__); }

pt_annotation_t pt_span_get_annotations(pt_span_t span) try {
    if (!span || !span->ptr) return nullptr;
    if (span == noop_span_sentinel()) return noop_annotation_sentinel();
    return make_annotation_handle(span->ptr->GetAnnotations());
} catch (...) { pt_handle_exception(__func__); return nullptr; }

// ============================================================================
// SpanEvent operations
// ============================================================================

void pt_span_event_destroy(pt_span_event_t se) try {
    if (se == noop_span_event_sentinel()) return;  // static sentinel — never owned
    delete se;
} catch (...) { pt_handle_exception(__func__); }

void pt_span_event_set_service_type(pt_span_event_t se, int32_t service_type) try {
    if (se && se->ptr) se->ptr->SetServiceType(service_type);
} catch (...) { pt_handle_exception(__func__); }

void pt_span_event_set_operation_name(pt_span_event_t se, const char* operation) try {
    if (se && se->ptr && operation) se->ptr->SetOperationName(operation);
} catch (...) { pt_handle_exception(__func__); }

void pt_span_event_set_start_time_ms(pt_span_event_t se, int64_t ms_since_epoch) try {
    if (se && se->ptr) {
        se->ptr->SetStartTime(ms_to_time_point(ms_since_epoch));
    }
} catch (...) { pt_handle_exception(__func__); }

void pt_span_event_set_destination(pt_span_event_t se, const char* dest) try {
    if (se && se->ptr && dest) se->ptr->SetDestination(dest);
} catch (...) { pt_handle_exception(__func__); }

void pt_span_event_set_end_point(pt_span_event_t se, const char* end_point) try {
    if (se && se->ptr && end_point) se->ptr->SetEndPoint(end_point);
} catch (...) { pt_handle_exception(__func__); }

void pt_span_event_set_error(pt_span_event_t se, const char* error_message) try {
    if (se && se->ptr && error_message) se->ptr->SetError(error_message);
} catch (...) { pt_handle_exception(__func__); }

void pt_span_event_set_error_named(pt_span_event_t se, const char* error_name,
                                   const char* error_message) try {
    if (se && se->ptr && error_name && error_message) {
        se->ptr->SetError(error_name, error_message);
    }
} catch (...) { pt_handle_exception(__func__); }

void pt_span_event_set_error_with_callstack(pt_span_event_t se,
                                            const char* error_name,
                                            const char* error_message,
                                            const pt_callstack_reader_t* reader) try {
    if (!se || !se->ptr) return;
    const char* name = error_name    ? error_name    : "";
    const char* msg  = error_message ? error_message : "";
    if (reader) {
        CCallstackReader cpt_reader(reader);
        se->ptr->SetError(name, msg, cpt_reader);
    } else {
        se->ptr->SetError(name, msg);
    }
} catch (...) { pt_handle_exception(__func__); }

void pt_span_event_set_sql_query(pt_span_event_t se, const char* sql_query,
                                 const char* args) try {
    if (se && se->ptr) {
        se->ptr->SetSqlQuery(sql_query ? sql_query : "",
                             args      ? args      : "");
    }
} catch (...) { pt_handle_exception(__func__); }

void pt_span_event_record_header(pt_span_event_t se, pt_header_type_t which,
                                 const pt_header_reader_t* reader) try {
    if (!se || !se->ptr || !reader) return;
    CHeaderReader cpt_reader(reader);
    se->ptr->RecordHeader(static_cast<pinpoint::HeaderType>(which), cpt_reader);
} catch (...) { pt_handle_exception(__func__); }

pt_annotation_t pt_span_event_get_annotations(pt_span_event_t se) try {
    if (!se || !se->ptr) return nullptr;
    if (se == noop_span_event_sentinel()) return noop_annotation_sentinel();
    return make_annotation_handle(se->ptr->GetAnnotations());
} catch (...) { pt_handle_exception(__func__); return nullptr; }

// ============================================================================
// Annotation operations
// ============================================================================

void pt_annotation_destroy(pt_annotation_t anno) try {
    if (anno == noop_annotation_sentinel()) return;  // static sentinel — never owned
    delete anno;
} catch (...) { pt_handle_exception(__func__); }

void pt_annotation_append_int(pt_annotation_t anno, int32_t key, int32_t value) try {
    if (anno && anno->ptr) anno->ptr->AppendInt(key, value);
} catch (...) { pt_handle_exception(__func__); }

void pt_annotation_append_long(pt_annotation_t anno, int32_t key, int64_t value) try {
    if (anno && anno->ptr) anno->ptr->AppendLong(key, value);
} catch (...) { pt_handle_exception(__func__); }

void pt_annotation_append_string(pt_annotation_t anno, int32_t key, const char* value) try {
    if (anno && anno->ptr && value) anno->ptr->AppendString(key, value);
} catch (...) { pt_handle_exception(__func__); }

void pt_annotation_append_string_string(pt_annotation_t anno, int32_t key,
                                        const char* s1, const char* s2) try {
    if (anno && anno->ptr) {
        anno->ptr->AppendStringString(key, s1 ? s1 : "", s2 ? s2 : "");
    }
} catch (...) { pt_handle_exception(__func__); }

void pt_annotation_append_int_string_string(pt_annotation_t anno, int32_t key,
                                            int i, const char* s1, const char* s2) try {
    if (anno && anno->ptr) {
        anno->ptr->AppendIntStringString(key, i, s1 ? s1 : "", s2 ? s2 : "");
    }
} catch (...) { pt_handle_exception(__func__); }

void pt_annotation_append_bytes_string_string(pt_annotation_t anno, int32_t key,
                                              const unsigned char* uid, int uid_len,
                                              const char* s1, const char* s2) try {
    if (!anno || !anno->ptr || !uid || uid_len <= 0) return;
    std::vector<unsigned char> v(uid, uid + uid_len);
    anno->ptr->AppendBytesStringString(key, std::move(v),
                                       s1 ? s1 : "", s2 ? s2 : "");
} catch (...) { pt_handle_exception(__func__); }

void pt_annotation_append_long_int_int_byte_byte_string(pt_annotation_t anno,
                                                        int32_t key,
                                                        int64_t l,
                                                        int32_t i1, int32_t i2,
                                                        int32_t b1, int32_t b2,
                                                        const char* s) try {
    if (anno && anno->ptr) {
        anno->ptr->AppendLongIntIntByteByteString(key, l, i1, i2, b1, b2,
                                                  s ? s : "");
    }
} catch (...) { pt_handle_exception(__func__); }

// ============================================================================
// HTTP helper functions
// ============================================================================

void pt_trace_http_server_request(pt_span_t span,
                                  const char* remote_addr,
                                  const char* endpoint,
                                  const pt_header_reader_t* request_reader) try {
    if (!span || !span->ptr || !request_reader) return;
    CHeaderReader cpt_req(request_reader);
    pinpoint::helper::TraceHttpServerRequest(span->ptr,
                                             remote_addr ? remote_addr : "",
                                             endpoint    ? endpoint    : "",
                                             cpt_req);
} catch (...) { pt_handle_exception(__func__); }

void pt_trace_http_server_request_with_cookie(pt_span_t span,
                                              const char* remote_addr,
                                              const char* endpoint,
                                              const pt_header_reader_t* request_reader,
                                              const pt_header_reader_t* cookie_reader) try {
    if (!span || !span->ptr || !request_reader || !cookie_reader) return;
    CHeaderReader cpt_req(request_reader);
    CHeaderReader cpt_cookie(cookie_reader);
    pinpoint::helper::TraceHttpServerRequest(span->ptr,
                                             remote_addr ? remote_addr : "",
                                             endpoint    ? endpoint    : "",
                                             cpt_req, cpt_cookie);
} catch (...) { pt_handle_exception(__func__); }

void pt_trace_http_server_response(pt_span_t span,
                                   const char* url_pattern,
                                   const char* method,
                                   int status_code,
                                   const pt_header_reader_t* response_reader) try {
    if (!span || !span->ptr || !response_reader) return;
    CHeaderReader cpt_resp(response_reader);
    pinpoint::helper::TraceHttpServerResponse(span->ptr,
                                              url_pattern ? url_pattern : "",
                                              method      ? method      : "",
                                              status_code,
                                              cpt_resp);
} catch (...) { pt_handle_exception(__func__); }

void pt_trace_http_client_request(pt_span_event_t se,
                                  const char* host,
                                  const char* url,
                                  const pt_header_reader_t* request_reader) try {
    if (!se || !se->ptr || !request_reader) return;
    CHeaderReader cpt_req(request_reader);
    pinpoint::helper::TraceHttpClientRequest(se->ptr,
                                             host ? host : "",
                                             url  ? url  : "",
                                             cpt_req);
} catch (...) { pt_handle_exception(__func__); }

void pt_trace_http_client_request_with_cookie(pt_span_event_t se,
                                              const char* host,
                                              const char* url,
                                              const pt_header_reader_t* request_reader,
                                              const pt_header_reader_t* cookie_reader) try {
    if (!se || !se->ptr || !request_reader || !cookie_reader) return;
    CHeaderReader cpt_req(request_reader);
    CHeaderReader cpt_cookie(cookie_reader);
    pinpoint::helper::TraceHttpClientRequest(se->ptr,
                                             host ? host : "",
                                             url  ? url  : "",
                                             cpt_req, cpt_cookie);
} catch (...) { pt_handle_exception(__func__); }

void pt_trace_http_client_response(pt_span_event_t se,
                                   int status_code,
                                   const pt_header_reader_t* response_reader) try {
    if (!se || !se->ptr || !response_reader) return;
    CHeaderReader cpt_resp(response_reader);
    pinpoint::helper::TraceHttpClientResponse(se->ptr, status_code, cpt_resp);
} catch (...) { pt_handle_exception(__func__); }
