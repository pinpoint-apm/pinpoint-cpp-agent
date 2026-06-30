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
 * Agent and span handles wrap C++ shared_ptr instances. Span-event and
 * annotation handles are non-owning: their pointees are owned by the parent
 * span/span event and must not be used after that owner is ended or destroyed.
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
#include <utility>
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

template <typename F>
static void pt_api_call(const char* func, F&& fn) noexcept {
    try {
        std::forward<F>(fn)();
    } catch (...) {
        pt_handle_exception(func);
    }
}

template <typename R, typename F>
static R pt_api_call(const char* func, R fallback, F&& fn) noexcept {
    try {
        return std::forward<F>(fn)();
    } catch (...) {
        pt_handle_exception(func);
        return fallback;
    }
}

template <typename Handle, typename F>
static void pt_handle_call(Handle handle, F&& fn) {
    if (handle && handle->ptr) {
        std::forward<F>(fn)(handle);
    }
}

template <typename Handle, typename R, typename F>
static R pt_handle_call(Handle handle, R fallback, F&& fn) {
    if (!handle || !handle->ptr) {
        return fallback;
    }
    return std::forward<F>(fn)(handle);
}

template <typename Handle, typename R, typename F>
static R pt_handle_call_or_noop(Handle handle, Handle noop_handle, R noop_result,
                                R fallback, F&& fn) {
    return pt_handle_call(handle, fallback, [&](Handle valid) {
        if (valid == noop_handle) {
            return noop_result;
        }
        return std::forward<F>(fn)(valid);
    });
}

template <typename Handle>
static void destroy_handle(Handle handle, Handle noop_handle) {
    if (handle == noop_handle) return;  // static sentinel — never owned
    delete handle;
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
// The C++ layer treats noop work as free. To preserve that at the C boundary we
// hand back one static sentinel handle per noop type so hot disabled paths skip
// handle allocation/free. Agent/span sentinels keep the shared noop owners; the
// event/annotation sentinels hold process-lifetime raw pointers owned by Noop.
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
    if (ptr == noop_span_event_sentinel()->ptr) return noop_span_event_sentinel();
    return new pt_span_event_s{ptr};
}

static pt_annotation_t make_annotation_handle(pinpoint::AnnotationPtr ptr) {
    if (!ptr) return nullptr;
    if (ptr == noop_annotation_sentinel()->ptr) return noop_annotation_sentinel();
    return new pt_annotation_s{ptr};
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
        const char* k_ptr = nullptr;
        std::string k_str;
        if (!key.empty() && key.data()[key.size()] == '\0') {
            k_ptr = key.data();
        } else {
            k_str = std::string(key);
            k_ptr = k_str.c_str();
        }
        const char* v = r_->get(r_->userdata, k_ptr);
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
        const char* k_ptr = nullptr;
        std::string k_str;
        if (!key.empty() && key.data()[key.size()] == '\0') {
            k_ptr = key.data();
        } else {
            k_str = std::string(key);
            k_ptr = k_str.c_str();
        }
        const char* v = r_->get(r_->userdata, k_ptr);
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
        const char* k_ptr = nullptr;
        std::string k_str;
        if (!key.empty() && key.data()[key.size()] == '\0') {
            k_ptr = key.data();
        } else {
            k_str = std::string(key);
            k_ptr = k_str.c_str();
        }

        const char* v_ptr = nullptr;
        std::string v_str;
        if (!value.empty() && value.data()[value.size()] == '\0') {
            v_ptr = value.data();
        } else {
            v_str = std::string(value);
            v_ptr = v_str.c_str();
        }
        w_->set(w_->userdata, k_ptr, v_ptr);
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

void pt_set_config_file_path(const char* config_file_path) {
    pt_api_call(__func__, [&] {
        if (config_file_path) {
            pinpoint::SetConfigFilePath(config_file_path);
        }
    });
}

void pt_set_config_string(const char* config_string) {
    pt_api_call(__func__, [&] {
        if (config_string) {
            pinpoint::SetConfigString(config_string);
        }
    });
}

// ============================================================================
// Agent lifecycle
// ============================================================================

pt_agent_t pt_create_agent(void) {
    return pt_api_call(__func__, static_cast<pt_agent_t>(nullptr), [] {
        return make_agent_handle(pinpoint::CreateAgent());
    });
}

pt_agent_t pt_create_agent_with_server_metadata(const char* server_info,
                                                const char* const* args,
                                                int args_count,
                                                const char* const* libs,
                                                int libs_count) {
    return pt_api_call(__func__, static_cast<pt_agent_t>(nullptr), [&] {
        return make_agent_handle(server_info
            ? pinpoint::CreateAgent(server_info,
                                    to_string_vector(args, args_count),
                                    to_string_vector(libs, libs_count))
            : pinpoint::CreateAgent());
    });
}

pt_agent_t pt_create_agent_with_type(int32_t app_type) {
    return pt_api_call(__func__, static_cast<pt_agent_t>(nullptr), [&] {
        return make_agent_handle(pinpoint::CreateAgent(app_type));
    });
}

pt_agent_t pt_create_agent_with_type_and_server_metadata(int32_t app_type,
                                                         const char* server_info,
                                                         const char* const* args,
                                                         int args_count,
                                                         const char* const* libs,
                                                         int libs_count) {
    return pt_api_call(__func__, static_cast<pt_agent_t>(nullptr), [&] {
        return make_agent_handle(server_info
            ? pinpoint::CreateAgent(app_type,
                                    server_info,
                                    to_string_vector(args, args_count),
                                    to_string_vector(libs, libs_count))
            : pinpoint::CreateAgent(app_type));
    });
}

pt_agent_t pt_global_agent(void) {
    return pt_api_call(__func__, static_cast<pt_agent_t>(nullptr), [] {
        // A missing global agent collapses to the shared noop-agent sentinel; an
        // existing one is wrapped in a handle whose shared_ptr keeps it alive
        // regardless of whether the caller ever calls pt_agent_destroy().
        return make_agent_handle(pinpoint::GlobalAgent());
    });
}

void pt_agent_destroy(pt_agent_t agent) {
    pt_api_call(__func__, [&] {
        destroy_handle(agent, noop_agent_sentinel());
    });
}

int pt_agent_is_enabled(pt_agent_t agent) {
    return pt_api_call(__func__, 0, [&] {
        return pt_handle_call(agent, 0, [](pt_agent_t valid) {
            return valid->ptr->Enable() ? 1 : 0;
        });
    });
}

void pt_agent_shutdown(pt_agent_t agent) {
    pt_api_call(__func__, [&] {
        pt_handle_call(agent, [](pt_agent_t valid) {
            valid->ptr->Shutdown();
        });
    });
}

// ============================================================================
// Span creation
// ============================================================================

pt_span_t pt_agent_new_span(pt_agent_t agent, const char* operation,
                            const char* rpc_point) {
    return pt_api_call(__func__, static_cast<pt_span_t>(nullptr), [&] {
        // The noop agent only ever makes noop spans — skip the call (and the
        // singleton refcount churn it would trigger) and hand back the sentinel.
        return pt_handle_call_or_noop(agent, noop_agent_sentinel(),
                                      static_cast<pt_span_t>(noop_span_sentinel()),
                                      static_cast<pt_span_t>(nullptr),
                                      [&](pt_agent_t valid) {
            return make_span_handle(valid->ptr->NewSpan(operation ? operation : "",
                                                        rpc_point  ? rpc_point  : ""));
        });
    });
}

pt_span_t pt_agent_new_span_with_reader(pt_agent_t agent, const char* operation,
                                        const char* rpc_point,
                                        const pt_context_reader_t* reader) {
    return pt_api_call(__func__, static_cast<pt_span_t>(nullptr), [&] {
        return pt_handle_call_or_noop(agent, noop_agent_sentinel(),
                                      static_cast<pt_span_t>(noop_span_sentinel()),
                                      static_cast<pt_span_t>(nullptr),
                                      [&](pt_agent_t valid) {
            pinpoint::SpanPtr ptr;
            if (reader) {
                CContextReader cpt_reader(reader);
                ptr = valid->ptr->NewSpan(operation ? operation : "",
                                          rpc_point  ? rpc_point  : "",
                                          cpt_reader);
            } else {
                ptr = valid->ptr->NewSpan(operation ? operation : "",
                                          rpc_point  ? rpc_point  : "");
            }
            return make_span_handle(std::move(ptr));
        });
    });
}

pt_span_t pt_agent_new_span_with_method(pt_agent_t agent, const char* operation,
                                        const char* rpc_point, const char* method,
                                        const pt_context_reader_t* reader) {
    return pt_api_call(__func__, static_cast<pt_span_t>(nullptr), [&] {
        return pt_handle_call_or_noop(agent, noop_agent_sentinel(),
                                      static_cast<pt_span_t>(noop_span_sentinel()),
                                      static_cast<pt_span_t>(nullptr),
                                      [&](pt_agent_t valid) {
            pinpoint::SpanPtr ptr;
            if (reader) {
                CContextReader cpt_reader(reader);
                ptr = valid->ptr->NewSpan(operation ? operation : "",
                                          rpc_point  ? rpc_point  : "",
                                          method     ? method     : "",
                                          cpt_reader);
            } else {
                ptr = valid->ptr->NewSpan(operation ? operation : "",
                                          rpc_point  ? rpc_point  : "");
            }
            return make_span_handle(std::move(ptr));
        });
    });
}

// ============================================================================
// Span operations
// ============================================================================

void pt_span_destroy(pt_span_t span) {
    pt_api_call(__func__, [&] {
        destroy_handle(span, noop_span_sentinel());
    });
}

pt_span_event_t pt_span_new_event(pt_span_t span, const char* operation) {
    return pt_api_call(__func__, static_cast<pt_span_event_t>(nullptr), [&] {
        return pt_handle_call_or_noop(span, noop_span_sentinel(),
                                      static_cast<pt_span_event_t>(noop_span_event_sentinel()),
                                      static_cast<pt_span_event_t>(nullptr),
                                      [&](pt_span_t valid) {
            return make_span_event_handle(
                valid->ptr->NewSpanEvent(operation ? operation : ""));
        });
    });
}

pt_span_event_t pt_span_new_event_with_type(pt_span_t span, const char* operation,
                                            int32_t service_type) {
    return pt_api_call(__func__, static_cast<pt_span_event_t>(nullptr), [&] {
        return pt_handle_call_or_noop(span, noop_span_sentinel(),
                                      static_cast<pt_span_event_t>(noop_span_event_sentinel()),
                                      static_cast<pt_span_event_t>(nullptr),
                                      [&](pt_span_t valid) {
            return make_span_event_handle(
                valid->ptr->NewSpanEvent(operation ? operation : "", service_type));
        });
    });
}

pt_span_event_t pt_span_get_event(pt_span_t span) {
    return pt_api_call(__func__, static_cast<pt_span_event_t>(nullptr), [&] {
        return pt_handle_call_or_noop(span, noop_span_sentinel(),
                                      static_cast<pt_span_event_t>(noop_span_event_sentinel()),
                                      static_cast<pt_span_event_t>(nullptr),
                                      [](pt_span_t valid) {
            return make_span_event_handle(valid->ptr->GetSpanEvent());
        });
    });
}

void pt_span_end_event(pt_span_t span) {
    pt_api_call(__func__, [&] {
        pt_handle_call(span, [](pt_span_t valid) {
            valid->ptr->EndSpanEvent();
        });
    });
}

void pt_span_end(pt_span_t span) {
    pt_api_call(__func__, [&] {
        pt_handle_call(span, [](pt_span_t valid) {
            valid->ptr->EndSpan();
        });
    });
}

pt_span_t pt_span_new_async_span(pt_span_t span, const char* async_operation) {
    return pt_api_call(__func__, static_cast<pt_span_t>(nullptr), [&] {
        return pt_handle_call_or_noop(span, noop_span_sentinel(),
                                      static_cast<pt_span_t>(noop_span_sentinel()),
                                      static_cast<pt_span_t>(nullptr),
                                      [&](pt_span_t valid) {
            return make_span_handle(
                valid->ptr->NewAsyncSpan(async_operation ? async_operation : ""));
        });
    });
}

void pt_span_inject_context(pt_span_t span, pt_context_writer_t* writer) {
    pt_api_call(__func__, [&] {
        if (!writer) return;
        pt_handle_call(span, [&](pt_span_t valid) {
            CContextWriter cpt_writer(writer);
            valid->ptr->InjectContext(cpt_writer);
        });
    });
}

void pt_span_extract_context(pt_span_t span, const pt_context_reader_t* reader) {
    pt_api_call(__func__, [&] {
        if (!reader) return;
        pt_handle_call(span, [&](pt_span_t valid) {
            CContextReader cpt_reader(reader);
            valid->ptr->ExtractContext(cpt_reader);
        });
    });
}

pt_trace_id_t pt_span_get_trace_id(pt_span_t span) {
    return pt_api_call(__func__, pt_trace_id_t{}, [&] {
        return pt_handle_call(span, pt_trace_id_t{}, [](pt_span_t valid) {
            pt_trace_id_t result{};
            fill_trace_id(&result, valid->ptr->GetTraceId());
            return result;
        });
    });
}

int64_t pt_span_get_span_id(pt_span_t span) {
    return pt_api_call(__func__, int64_t{0}, [&] {
        return pt_handle_call(span, int64_t{0}, [](pt_span_t valid) {
            return valid->ptr->GetSpanId();
        });
    });
}

int pt_span_is_sampled(pt_span_t span) {
    return pt_api_call(__func__, 0, [&] {
        return pt_handle_call(span, 0, [](pt_span_t valid) {
            return valid->ptr->IsSampled() ? 1 : 0;
        });
    });
}

void pt_span_set_service_type(pt_span_t span, int32_t service_type) {
    pt_api_call(__func__, [&] {
        pt_handle_call(span, [&](pt_span_t valid) {
            valid->ptr->SetServiceType(service_type);
        });
    });
}

void pt_span_set_start_time_ms(pt_span_t span, int64_t ms_since_epoch) {
    pt_api_call(__func__, [&] {
        pt_handle_call(span, [&](pt_span_t valid) {
            valid->ptr->SetStartTime(ms_to_time_point(ms_since_epoch));
        });
    });
}

void pt_span_set_remote_address(pt_span_t span, const char* address) {
    pt_api_call(__func__, [&] {
        if (!address) return;
        pt_handle_call(span, [&](pt_span_t valid) {
            valid->ptr->SetRemoteAddress(address);
        });
    });
}

void pt_span_set_end_point(pt_span_t span, const char* end_point) {
    pt_api_call(__func__, [&] {
        if (!end_point) return;
        pt_handle_call(span, [&](pt_span_t valid) {
            valid->ptr->SetEndPoint(end_point);
        });
    });
}

void pt_span_set_acceptor_host(pt_span_t span, const char* host) {
    pt_api_call(__func__, [&] {
        if (!host) return;
        pt_handle_call(span, [&](pt_span_t valid) {
            valid->ptr->SetAcceptorHost(host);
        });
    });
}

void pt_span_set_error(pt_span_t span, const char* error_message) {
    pt_api_call(__func__, [&] {
        if (!error_message) return;
        pt_handle_call(span, [&](pt_span_t valid) {
            valid->ptr->SetError(error_message);
        });
    });
}

void pt_span_set_error_named(pt_span_t span, const char* error_name,
                             const char* error_message) {
    pt_api_call(__func__, [&] {
        if (!error_name || !error_message) return;
        pt_handle_call(span, [&](pt_span_t valid) {
            valid->ptr->SetError(error_name, error_message);
        });
    });
}

void pt_span_set_status_code(pt_span_t span, int status_code) {
    pt_api_call(__func__, [&] {
        pt_handle_call(span, [&](pt_span_t valid) {
            valid->ptr->SetStatusCode(status_code);
        });
    });
}

void pt_span_set_url_stat(pt_span_t span, const char* url_pattern,
                          const char* method, int status_code) {
    pt_api_call(__func__, [&] {
        pt_handle_call(span, [&](pt_span_t valid) {
            valid->ptr->SetUrlStat(url_pattern ? url_pattern : "",
                                   method      ? method      : "",
                                   status_code);
        });
    });
}

void pt_span_set_logging(pt_span_t span, pt_context_writer_t* writer) {
    pt_api_call(__func__, [&] {
        if (!writer) return;
        pt_handle_call(span, [&](pt_span_t valid) {
            CContextWriter cpt_writer(writer);
            valid->ptr->SetLogging(cpt_writer);
        });
    });
}

void pt_span_record_header(pt_span_t span, pt_header_type_t which,
                           const pt_header_reader_t* reader) {
    pt_api_call(__func__, [&] {
        if (!reader) return;
        pt_handle_call(span, [&](pt_span_t valid) {
            CHeaderReader cpt_reader(reader);
            valid->ptr->RecordHeader(static_cast<pinpoint::HeaderType>(which), cpt_reader);
        });
    });
}

pt_annotation_t pt_span_get_annotations(pt_span_t span) {
    return pt_api_call(__func__, static_cast<pt_annotation_t>(nullptr), [&] {
        return pt_handle_call_or_noop(span, noop_span_sentinel(),
                                      static_cast<pt_annotation_t>(noop_annotation_sentinel()),
                                      static_cast<pt_annotation_t>(nullptr),
                                      [](pt_span_t valid) {
            return make_annotation_handle(valid->ptr->GetAnnotations());
        });
    });
}

// ============================================================================
// SpanEvent operations
// ============================================================================

void pt_span_event_destroy(pt_span_event_t se) {
    pt_api_call(__func__, [&] {
        destroy_handle(se, noop_span_event_sentinel());
    });
}

void pt_span_event_set_service_type(pt_span_event_t se, int32_t service_type) {
    pt_api_call(__func__, [&] {
        pt_handle_call(se, [&](pt_span_event_t valid) {
            valid->ptr->SetServiceType(service_type);
        });
    });
}

void pt_span_event_set_operation_name(pt_span_event_t se, const char* operation) {
    pt_api_call(__func__, [&] {
        if (!operation) return;
        pt_handle_call(se, [&](pt_span_event_t valid) {
            valid->ptr->SetOperationName(operation);
        });
    });
}

void pt_span_event_set_start_time_ms(pt_span_event_t se, int64_t ms_since_epoch) {
    pt_api_call(__func__, [&] {
        pt_handle_call(se, [&](pt_span_event_t valid) {
            valid->ptr->SetStartTime(ms_to_time_point(ms_since_epoch));
        });
    });
}

void pt_span_event_set_destination(pt_span_event_t se, const char* dest) {
    pt_api_call(__func__, [&] {
        if (!dest) return;
        pt_handle_call(se, [&](pt_span_event_t valid) {
            valid->ptr->SetDestination(dest);
        });
    });
}

void pt_span_event_set_end_point(pt_span_event_t se, const char* end_point) {
    pt_api_call(__func__, [&] {
        if (!end_point) return;
        pt_handle_call(se, [&](pt_span_event_t valid) {
            valid->ptr->SetEndPoint(end_point);
        });
    });
}

void pt_span_event_set_error(pt_span_event_t se, const char* error_message) {
    pt_api_call(__func__, [&] {
        if (!error_message) return;
        pt_handle_call(se, [&](pt_span_event_t valid) {
            valid->ptr->SetError(error_message);
        });
    });
}

void pt_span_event_set_error_named(pt_span_event_t se, const char* error_name,
                                   const char* error_message) {
    pt_api_call(__func__, [&] {
        if (!error_name || !error_message) return;
        pt_handle_call(se, [&](pt_span_event_t valid) {
            valid->ptr->SetError(error_name, error_message);
        });
    });
}

void pt_span_event_set_error_with_callstack(pt_span_event_t se,
                                            const char* error_name,
                                            const char* error_message,
                                            const pt_callstack_reader_t* reader) {
    pt_api_call(__func__, [&] {
        pt_handle_call(se, [&](pt_span_event_t valid) {
            const char* name = error_name    ? error_name    : "";
            const char* msg  = error_message ? error_message : "";
            if (reader) {
                CCallstackReader cpt_reader(reader);
                valid->ptr->SetError(name, msg, cpt_reader);
            } else {
                valid->ptr->SetError(name, msg);
            }
        });
    });
}

void pt_span_event_set_sql_query(pt_span_event_t se, const char* sql_query,
                                 const char* args) {
    pt_api_call(__func__, [&] {
        pt_handle_call(se, [&](pt_span_event_t valid) {
            valid->ptr->SetSqlQuery(sql_query ? sql_query : "",
                                    args      ? args      : "");
        });
    });
}

void pt_span_event_record_header(pt_span_event_t se, pt_header_type_t which,
                                 const pt_header_reader_t* reader) {
    pt_api_call(__func__, [&] {
        if (!reader) return;
        pt_handle_call(se, [&](pt_span_event_t valid) {
            CHeaderReader cpt_reader(reader);
            valid->ptr->RecordHeader(static_cast<pinpoint::HeaderType>(which), cpt_reader);
        });
    });
}

pt_annotation_t pt_span_event_get_annotations(pt_span_event_t se) {
    return pt_api_call(__func__, static_cast<pt_annotation_t>(nullptr), [&] {
        return pt_handle_call_or_noop(se, noop_span_event_sentinel(),
                                      static_cast<pt_annotation_t>(noop_annotation_sentinel()),
                                      static_cast<pt_annotation_t>(nullptr),
                                      [](pt_span_event_t valid) {
            return make_annotation_handle(valid->ptr->GetAnnotations());
        });
    });
}

// ============================================================================
// Annotation operations
// ============================================================================

void pt_annotation_destroy(pt_annotation_t anno) {
    pt_api_call(__func__, [&] {
        destroy_handle(anno, noop_annotation_sentinel());
    });
}

void pt_annotation_append_int(pt_annotation_t anno, int32_t key, int32_t value) {
    pt_api_call(__func__, [&] {
        pt_handle_call(anno, [&](pt_annotation_t valid) {
            valid->ptr->AppendInt(key, value);
        });
    });
}

void pt_annotation_append_long(pt_annotation_t anno, int32_t key, int64_t value) {
    pt_api_call(__func__, [&] {
        pt_handle_call(anno, [&](pt_annotation_t valid) {
            valid->ptr->AppendLong(key, value);
        });
    });
}

void pt_annotation_append_string(pt_annotation_t anno, int32_t key, const char* value) {
    pt_api_call(__func__, [&] {
        if (!value) return;
        pt_handle_call(anno, [&](pt_annotation_t valid) {
            valid->ptr->AppendString(key, value);
        });
    });
}

void pt_annotation_append_string_string(pt_annotation_t anno, int32_t key,
                                        const char* s1, const char* s2) {
    pt_api_call(__func__, [&] {
        pt_handle_call(anno, [&](pt_annotation_t valid) {
            valid->ptr->AppendStringString(key, s1 ? s1 : "", s2 ? s2 : "");
        });
    });
}

void pt_annotation_append_int_string_string(pt_annotation_t anno, int32_t key,
                                            int i, const char* s1, const char* s2) {
    pt_api_call(__func__, [&] {
        pt_handle_call(anno, [&](pt_annotation_t valid) {
            valid->ptr->AppendIntStringString(key, i, s1 ? s1 : "", s2 ? s2 : "");
        });
    });
}

void pt_annotation_append_sql_uid_string_string(pt_annotation_t anno, int32_t key,
                                              const unsigned char* uid, int uid_len,
                                              const char* s1, const char* s2) {
    pt_api_call(__func__, [&] {
        if (!uid || uid_len <= 0) return;
        pt_handle_call(anno, [&](pt_annotation_t valid) {
            pinpoint::SqlUid sql_uid{};
            // Only a fixed-size SQL UID is supported; reject any other length.
            if (static_cast<size_t>(uid_len) != sql_uid.size()) return;
            std::memcpy(sql_uid.data(), uid, sql_uid.size());
            valid->ptr->AppendSqlUidStringString(key, sql_uid,
                                                 s1 ? s1 : "", s2 ? s2 : "");
        });
    });
}

void pt_annotation_append_long_int_int_byte_byte_string(pt_annotation_t anno,
                                                        int32_t key,
                                                        int64_t l,
                                                        int32_t i1, int32_t i2,
                                                        int32_t b1, int32_t b2,
                                                        const char* s) {
    pt_api_call(__func__, [&] {
        pt_handle_call(anno, [&](pt_annotation_t valid) {
            valid->ptr->AppendLongIntIntByteByteString(key, l, i1, i2, b1, b2,
                                                       s ? s : "");
        });
    });
}

// ============================================================================
// HTTP helper functions
// ============================================================================

void pt_trace_http_server_request(pt_span_t span,
                                  const char* remote_addr,
                                  const char* endpoint,
                                  const pt_header_reader_t* request_reader) {
    pt_api_call(__func__, [&] {
        if (!request_reader) return;
        pt_handle_call(span, [&](pt_span_t valid) {
            CHeaderReader cpt_req(request_reader);
            pinpoint::helper::TraceHttpServerRequest(valid->ptr,
                                                     remote_addr ? remote_addr : "",
                                                     endpoint    ? endpoint    : "",
                                                     cpt_req);
        });
    });
}

void pt_trace_http_server_request_with_cookie(pt_span_t span,
                                              const char* remote_addr,
                                              const char* endpoint,
                                              const pt_header_reader_t* request_reader,
                                              const pt_header_reader_t* cookie_reader) {
    pt_api_call(__func__, [&] {
        if (!request_reader || !cookie_reader) return;
        pt_handle_call(span, [&](pt_span_t valid) {
            CHeaderReader cpt_req(request_reader);
            CHeaderReader cpt_cookie(cookie_reader);
            pinpoint::helper::TraceHttpServerRequest(valid->ptr,
                                                     remote_addr ? remote_addr : "",
                                                     endpoint    ? endpoint    : "",
                                                     cpt_req, cpt_cookie);
        });
    });
}

void pt_trace_http_server_response(pt_span_t span,
                                   const char* url_pattern,
                                   const char* method,
                                   int status_code,
                                   const pt_header_reader_t* response_reader) {
    pt_api_call(__func__, [&] {
        if (!response_reader) return;
        pt_handle_call(span, [&](pt_span_t valid) {
            CHeaderReader cpt_resp(response_reader);
            pinpoint::helper::TraceHttpServerResponse(valid->ptr,
                                                      url_pattern ? url_pattern : "",
                                                      method      ? method      : "",
                                                      status_code,
                                                      cpt_resp);
        });
    });
}

void pt_trace_http_client_request(pt_span_event_t se,
                                  const char* host,
                                  const char* url,
                                  const pt_header_reader_t* request_reader) {
    pt_api_call(__func__, [&] {
        if (!request_reader) return;
        pt_handle_call(se, [&](pt_span_event_t valid) {
            CHeaderReader cpt_req(request_reader);
            pinpoint::helper::TraceHttpClientRequest(valid->ptr,
                                                     host ? host : "",
                                                     url  ? url  : "",
                                                     cpt_req);
        });
    });
}

void pt_trace_http_client_request_with_cookie(pt_span_event_t se,
                                              const char* host,
                                              const char* url,
                                              const pt_header_reader_t* request_reader,
                                              const pt_header_reader_t* cookie_reader) {
    pt_api_call(__func__, [&] {
        if (!request_reader || !cookie_reader) return;
        pt_handle_call(se, [&](pt_span_event_t valid) {
            CHeaderReader cpt_req(request_reader);
            CHeaderReader cpt_cookie(cookie_reader);
            pinpoint::helper::TraceHttpClientRequest(valid->ptr,
                                                     host ? host : "",
                                                     url  ? url  : "",
                                                     cpt_req, cpt_cookie);
        });
    });
}

void pt_trace_http_client_response(pt_span_event_t se,
                                   int status_code,
                                   const pt_header_reader_t* response_reader) {
    pt_api_call(__func__, [&] {
        if (!response_reader) return;
        pt_handle_call(se, [&](pt_span_event_t valid) {
            CHeaderReader cpt_resp(response_reader);
            pinpoint::helper::TraceHttpClientResponse(valid->ptr, status_code, cpt_resp);
        });
    });
}
