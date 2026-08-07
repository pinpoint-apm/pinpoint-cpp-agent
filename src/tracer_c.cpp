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
 * Agent and span handles are opaque registry tokens; each live registry entry
 * owns a heap wrapper containing a C++ shared_ptr. Span-event handles are
 * non-owning raw pointers cast directly to the opaque handle type — creating
 * one allocates nothing and destroying one is a no-op. Their pointees are
 * owned by the parent span and must not be used after that owner is
 * destroyed.
 *
 * Adapter objects (CContextReader, CHeaderReader, CContextWriter,
 * CCallstackReader) are constructed on the stack at each call site. Their
 * string conversion buffers reuse capacity but may allocate when they grow.
 */

#include "pinpoint/tracer_c.h"
#include "pinpoint/tracer.h"

#include "logging.h"
#include "noop.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
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

// ============================================================================
// Owned-handle registry
//
// Agent and span handles are the only heap-owned handles, and the classic C
// misuse — destroying one twice — would be a double delete (heap corruption
// inside the host application). Tracking raw wrapper addresses is insufficient:
// after one is deleted, the allocator can reuse that address for a new handle,
// and a stale second destroy would then delete the new live handle (ABA).
//
// Expose monotonically-generated, never-reused odd pointer tokens instead.
// They are opaque identity values and are never dereferenced; the registry maps
// each token to a shared wrapper. A lookup copies that shared_ptr under the
// shard lock, so a concurrent destroy can remove the token without freeing the
// wrapper out from under a call already in progress. Odd values cannot collide
// with the aligned addresses of the two leaked noop sentinels.
// ============================================================================

struct pt_agent_s { pinpoint::AgentPtr ptr; };
struct pt_span_s  { pinpoint::SpanPtr  ptr; };
static_assert(alignof(pt_agent_s) > 1 && alignof(pt_span_s) > 1,
              "noop sentinel addresses must not use the odd token tag");

static pt_agent_s* noop_agent_sentinel() {
    // Heap-allocated and leaked for the same shutdown-order reason as the
    // handle registries and id counter below: a host may operate on a noop
    // handle from a global destructor / atexit during process exit. A value
    // static would be destroyed first, turning that into use-after-destruction.
    static pt_agent_s* sentinel = new pt_agent_s{pinpoint::noopAgent()};
    return sentinel;
}

static pt_span_s* noop_span_sentinel() {
    static pt_span_s* sentinel = new pt_span_s{pinpoint::noopSpan()};
    return sentinel;
}

namespace {
std::atomic<uintptr_t>& next_owned_handle_id() {
    // Heap-allocated for the same shutdown-order reason as the registries:
    // C handles may be created from a host global destructor.
    static auto* next = new std::atomic<uintptr_t>{1};
    return *next;
}

template <typename Handle>
Handle make_owned_handle_token() {
    constexpr auto kMaxId = std::numeric_limits<uintptr_t>::max() >> 1;
    auto& next = next_owned_handle_id();
    auto id = next.load(std::memory_order_relaxed);
    while (true) {
        if (id > kMaxId) {
            throw std::overflow_error("C handle token space exhausted");
        }
        if (next.compare_exchange_weak(id, id + 1,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
            return reinterpret_cast<Handle>((id << 1) | uintptr_t{1});
        }
    }
}

template <typename Handle, typename Wrapper>
class OwnedHandleRegistry {
public:
    using Entry = std::shared_ptr<Wrapper>;

    Handle insert(Entry entry) {
        const auto handle = make_owned_handle_token<Handle>();
        auto& shard = shard_for(handle);
        std::lock_guard<std::shared_mutex> lock(shard.mutex);
        const auto [pos, inserted] = shard.live.emplace(handle, std::move(entry));
        (void)pos;
        if (!inserted) {
            throw std::logic_error("duplicate C handle token");
        }
        return handle;
    }

    Entry find(Handle handle) {
        auto& shard = shard_for(handle);
        // Shared lock: find() runs on every span-level C call (5-15 times per
        // traced request, from every request thread), while insert/erase run
        // once per span. Concurrent lookups on the same shard must not
        // serialize behind each other; copying the shared_ptr is safe under
        // the shared lock because erase() only detaches the entry from the
        // map — the wrapper itself stays alive through the copied reference.
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        const auto found = shard.live.find(handle);
        return found == shard.live.end() ? Entry{} : found->second;
    }

    // Removes and returns the shared wrapper. Moving it out before erasing
    // ensures destruction happens after the shard mutex is released.
    Entry erase(Handle handle) {
        auto& shard = shard_for(handle);
        Entry removed;
        {
            std::lock_guard<std::shared_mutex> lock(shard.mutex);
            const auto found = shard.live.find(handle);
            if (found == shard.live.end()) {
                return {};
            }
            removed = std::move(found->second);
            shard.live.erase(found);
        }
        return removed;
    }

private:
    // alignas(64) keeps neighboring shards on separate cache lines, like the
    // active-span and queue shards, so uncorrelated handles do not ping-pong
    // one line between request threads.
    struct alignas(64) Shard {
        std::shared_mutex mutex;
        std::unordered_map<Handle, Entry> live;
    };
    // 32 shards, in line with the other per-request sharded structures
    // (caches use 16, active spans 64): tokens are sequential, so shard_for
    // spreads concurrently-live handles round-robin and the count mostly
    // bounds same-shard collisions between unrelated request threads.
    static constexpr size_t kShardCount = 32;

    Shard& shard_for(Handle handle) {
        return shards_[(reinterpret_cast<uintptr_t>(handle) >> 1) % kShardCount];
    }

    Shard shards_[kShardCount];
};

using AgentHandleRegistry = OwnedHandleRegistry<pt_agent_t, pt_agent_s>;
using SpanHandleRegistry = OwnedHandleRegistry<pt_span_t, pt_span_s>;

AgentHandleRegistry& agent_handle_registry() {
    static auto* registry = new AgentHandleRegistry();
    return *registry;
}

SpanHandleRegistry& span_handle_registry() {
    static auto* registry = new SpanHandleRegistry();
    return *registry;
}
}  // namespace

// Overload-selected per handle type so the two pt_handle_call templates
// below stay generic over agents and spans.
static auto& handle_registry_for(pt_agent_t) { return agent_handle_registry(); }
static auto& handle_registry_for(pt_span_t) { return span_handle_registry(); }
static pt_agent_t noop_sentinel_for(pt_agent_t) { return noop_agent_sentinel(); }
static pt_span_t noop_sentinel_for(pt_span_t) { return noop_span_sentinel(); }

template <typename Handle, typename F>
static void pt_handle_call(Handle handle, F&& fn) {
    if (handle == nullptr) return;
    if (handle == noop_sentinel_for(handle)) {
        std::forward<F>(fn)(handle);
        return;
    }
    if (auto owned = handle_registry_for(handle).find(handle); owned && owned->ptr) {
        std::forward<F>(fn)(owned.get());
    }
}

template <typename Handle, typename R, typename F>
static R pt_handle_call(Handle handle, R fallback, F&& fn) {
    if (handle == nullptr) return fallback;
    if (handle == noop_sentinel_for(handle)) {
        return std::forward<F>(fn)(handle);
    }
    auto owned = handle_registry_for(handle).find(handle);
    return owned && owned->ptr ? std::forward<F>(fn)(owned.get()) : fallback;
}

template <typename Handle, typename R, typename F>
static R pt_handle_call_or_noop(Handle handle, Handle noop_handle, R noop_result,
                                R fallback, F&& fn) {
    if (handle == noop_handle) {
        return noop_result;
    }
    return pt_handle_call(handle, fallback, std::forward<F>(fn));
}

static void destroy_handle(pt_agent_t handle, pt_agent_t noop_handle) {
    if (handle == nullptr || handle == noop_handle) return;
    if (!agent_handle_registry().erase(handle)) {
        LOG_WARN("destroying an unknown or already-destroyed handle: ignored");
    }
}

static void destroy_handle(pt_span_t handle, pt_span_t noop_handle) {
    if (handle == nullptr || handle == noop_handle) return;
    if (!span_handle_registry().erase(handle)) {
        LOG_WARN("destroying an unknown or already-destroyed handle: ignored");
    }
}

// ============================================================================
// Opaque handle definitions
//
// Agent and span handle values are the registry tokens described above; their
// registry-owned wrappers carry the shared_ptr that keeps the C++ object alive.
//
// Span-event handles wrap non-owning raw pointers: the handle IS the pointer,
// reinterpret_cast to the opaque handle type. pt_span_event_s is never
// defined — the cast is only ever reversed, never dereferenced through the
// handle type. This keeps per-event handle traffic allocation-free (including
// the per-span disabled event, which cannot be a static sentinel), makes
// destroy a no-op, and collapses the shared noop/unsampled singletons to one
// handle value per pointee without any sentinel bookkeeping. A null pointer
// maps to a null handle.
// ============================================================================

static pt_span_event_t make_span_event_handle(pinpoint::SpanEventPtr ptr) {
    return reinterpret_cast<pt_span_event_t>(ptr);
}

static pinpoint::SpanEventPtr span_event_of(pt_span_event_t handle) {
    return reinterpret_cast<pinpoint::SpanEventPtr>(handle);
}

// pt_handle_call overload for the pointer-cast handle: unwrap and
// null-check, then hand the lambda the C++ pointer itself.
template <typename F>
static void pt_handle_call(pt_span_event_t handle, F&& fn) {
    if (auto* se = span_event_of(handle)) {
        std::forward<F>(fn)(se);
    }
}

// ============================================================================
// Static noop sentinel handles (agent and span only)
//
// The C++ layer treats noop work as free. To preserve that at the C boundary we
// hand back one static sentinel handle per noop owner type so hot disabled
// paths skip handle allocation/free and refcount churn on the shared noop
// singletons. Event handles need no sentinels — they are pointer
// casts, so singleton pointees collapse to one handle value by construction.
//
// Lazy function-local statics give thread-safe initialization (C++11) and the
// correct teardown order: each sentinel is constructed after the noop singleton
// it references (noopXxx() is called during the sentinel's own init), so it is
// destroyed first and its reference keeps the singleton alive until then.
// ============================================================================

// Wrap a C++ result in a fresh registry token, unless it is the shared noop
// singleton — in which case hand back the static sentinel and let the local
// reference drop. A null result maps to a null handle. The pointee comparison
// is exact: a real (live) object can never share an address with the live noop
// singleton, so this never misclassifies a span that should record.

static pt_agent_t make_agent_handle(pinpoint::AgentPtr ptr) {
    if (!ptr) return nullptr;
    if (ptr.get() == noop_agent_sentinel()->ptr.get()) return noop_agent_sentinel();
    return agent_handle_registry().insert(
        std::make_shared<pt_agent_s>(pt_agent_s{std::move(ptr)}));
}

static pt_span_t make_span_handle(pinpoint::SpanPtr ptr) {
    if (!ptr) return nullptr;
    if (ptr.get() == noop_span_sentinel()->ptr.get()) return noop_span_sentinel();
    return span_handle_registry().insert(
        std::make_shared<pt_span_s>(pt_span_s{std::move(ptr)}));
}

// ============================================================================
// Trampoline helpers
//
// C++ std::function closures cannot be converted to plain C function pointers.
// We use a small context struct placed on the caller's stack together with a
// file-scope trampoline function, so the trampoline bridge does not allocate
// a separate callback context.
// ============================================================================

// Both trampolines are invoked from inside the USER's C for_each loop, so a
// C++ exception escaping here would unwind through C stack frames — undefined
// behaviour, and an immediate std::terminate when the C code was compiled
// without unwind tables. The pt_* entry firewall cannot help: those C frames
// sit in the middle of the stack. Catch everything at this boundary instead.

struct ForEachCtx {
    const std::function<bool(std::string_view, std::string_view)>* cb;
    bool failed{false};
};

static int for_each_trampoline(const char* key, const char* value, void* userdata) {
    auto* ctx = static_cast<ForEachCtx*>(userdata);
    // Return 0 to continue, non-zero to stop — mirrors the C callback contract.
    // Guard NULLs like the callstack trampoline: a user callback naturally
    // passes NULL for a missing value, and string_view(nullptr) crashes.
    try {
        return (*ctx->cb)(key ? key : "", value ? value : "") ? 0 : 1;
    } catch (...) {
        ctx->failed = true;
        return 1;
    }
}

struct CallstackForEachCtx {
    const std::function<void(std::string_view, std::string_view, std::string_view, int)>* cb;
    bool failed{false};
};

static void callstack_foreach_trampoline(const char* mod, const char* fn,
                                         const char* file, int line,
                                         void* userdata) {
    auto* ctx = static_cast<CallstackForEachCtx*>(userdata);
    // No stop channel in this C callback contract: skip remaining frames
    // after a failure instead of throwing through the caller's C frames.
    if (ctx->failed) {
        return;
    }
    try {
        (*ctx->cb)(mod ? mod : "", fn ? fn : "", file ? file : "", line);
    } catch (...) {
        ctx->failed = true;
    }
}

// ============================================================================
// C++ adapter classes — stack-allocated callback bridges
// ============================================================================

// Returns a NUL-terminated pointer for `sv`, copying into `storage`. Probing
// sv.data()[sv.size()] to skip the copy would read one byte past the view —
// out of bounds for a substring view ending exactly at the end of its buffer.
// Callers pass an adapter member buffer, so later calls reuse retained
// capacity; an assignment allocates only when the new key/value exceeds the
// buffer's current capacity.
static const char* to_c_str(std::string_view sv, std::string& storage) {
    storage.assign(sv);
    return storage.c_str();
}

/**
 * Wraps pt_context_reader_t as a pinpoint::TraceContextReader.
 */
class CContextReader final : public pinpoint::TraceContextReader {
public:
    explicit CContextReader(const pt_context_reader_t* r) : r_(r) {}

    std::optional<std::string_view> Get(std::string_view key) const override {
        if (!r_ || !r_->get) return std::nullopt;
        // Zero-copy result: pt_reader_get_fn's contract already requires the
        // returned pointer to stay valid until the next call on the same
        // carrier, which matches the TraceContextReader::Get view-lifetime
        // contract exactly.
        const char* v = r_->get(r_->userdata, to_c_str(key, key_buf_));
        return v ? std::optional<std::string_view>(v) : std::nullopt;
    }

private:
    const pt_context_reader_t* r_;
    // Reused across Get calls: context extraction looks up ~9 Pinpoint header
    // keys per span, several of which exceed the SSO buffer.
    mutable std::string key_buf_;
};

/**
 * Wraps pt_header_reader_t as a pinpoint::HeaderReader.
 */
class CHeaderReader final : public pinpoint::HeaderReader {
public:
    explicit CHeaderReader(const pt_header_reader_t* r) : r_(r) {}

    std::optional<std::string_view> Get(std::string_view key) const override {
        if (!r_ || !r_->get) return std::nullopt;
        // Zero-copy result: see CContextReader::Get — the C contract matches
        // the TraceContextReader::Get view-lifetime contract.
        const char* v = r_->get(r_->userdata, to_c_str(key, key_buf_));
        return v ? std::optional<std::string_view>(v) : std::nullopt;
    }

    void ForEach(std::function<bool(std::string_view, std::string_view)> cb) const override {
        if (!r_ || !r_->for_each) return;
        ForEachCtx ctx{&cb};
        r_->for_each(r_->userdata, for_each_trampoline, &ctx);
        if (ctx.failed) {
            LOG_WARN("header for_each callback aborted by exception");
        }
    }

private:
    const pt_header_reader_t* r_;
    // Reused across Get calls; see CContextReader::key_buf_.
    mutable std::string key_buf_;
};

/**
 * Wraps pt_context_writer_t as a pinpoint::TraceContextWriter.
 */
class CContextWriter final : public pinpoint::TraceContextWriter {
public:
    explicit CContextWriter(pt_context_writer_t* w) : w_(w) {}

    void Set(std::string_view key, std::string_view value) override {
        if (!w_ || !w_->set) return;
        w_->set(w_->userdata, to_c_str(key, key_buf_), to_c_str(value, val_buf_));
    }

private:
    pt_context_writer_t* w_;
    // Reused across Set calls: InjectContext/SetLogging write ~9 headers per
    // outbound call, and values like the serialized trace id exceed the SSO
    // buffer.
    std::string key_buf_;
    std::string val_buf_;
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
        if (ctx.failed) {
            LOG_WARN("callstack for_each callback aborted by exception; remaining frames dropped");
        }
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
// Agent options
// ============================================================================

// Opaque owner of the C++ options mirrored by the pt_agent_options_* setters.
struct pt_agent_options_s {
    pinpoint::AgentOptions options;
};

pt_agent_options_t pt_agent_options_new(void) {
    return pt_api_call(__func__, static_cast<pt_agent_options_t>(nullptr), [] {
        return new pt_agent_options_s();
    });
}

void pt_agent_options_free(pt_agent_options_t options) {
    pt_api_call(__func__, [&] {
        delete options;
    });
}

void pt_agent_options_set_config_file(pt_agent_options_t options, const char* path) {
    pt_api_call(__func__, [&] {
        if (options) {
            options->options.config_file_path = path ? path : "";
        }
    });
}

void pt_agent_options_set_config_yaml(pt_agent_options_t options, const char* yaml) {
    pt_api_call(__func__, [&] {
        if (options) {
            options->options.config_yaml = yaml ? yaml : "";
        }
    });
}

void pt_agent_options_set_env_prefix(pt_agent_options_t options, const char* prefix) {
    pt_api_call(__func__, [&] {
        if (options) {
            options->options.env_prefix = prefix ? prefix : "";
        }
    });
}

void pt_agent_options_set_app_type(pt_agent_options_t options, int32_t app_type) {
    pt_api_call(__func__, [&] {
        if (options) {
            options->options.app_type = app_type;
        }
    });
}

void pt_agent_options_set_server_metadata(pt_agent_options_t options,
                                          const char* server_info,
                                          const char* const* args,
                                          int args_count,
                                          const char* const* libs,
                                          int libs_count) {
    pt_api_call(__func__, [&] {
        if (!options) {
            return;
        }
        if (server_info) {
            options->options.server_info = server_info;
        }
        options->options.args = to_string_vector(args, args_count);
        options->options.libs = to_string_vector(libs, libs_count);
    });
}

// ============================================================================
// Agent lifecycle
// ============================================================================

int pt_start_agent(pt_agent_options_t options) {
    return pt_api_call(__func__, 0, [&] {
        const bool started = options
            ? pinpoint::StartAgent(options->options)
            : pinpoint::StartAgent();
        return started ? 1 : 0;
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
                // The C++ API only accepts a method together with a reader;
                // use an empty reader so a NULL carrier doesn't drop the method.
                pinpoint::NoopTraceContextReader noop_reader;
                ptr = valid->ptr->NewSpan(operation ? operation : "",
                                          rpc_point  ? rpc_point  : "",
                                          method     ? method     : "",
                                          noop_reader);
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

// Event handles are pointer casts, so unlike span creation there is no noop
// shortcut here: the virtual call on the (stateless) noop span is already
// allocation-free and its singleton result collapses to one handle value.

pt_span_event_t pt_span_new_event(pt_span_t span, const char* operation) {
    return pt_api_call(__func__, static_cast<pt_span_event_t>(nullptr), [&] {
        return pt_handle_call(span, static_cast<pt_span_event_t>(nullptr),
                              [&](pt_span_t valid) {
            return make_span_event_handle(
                valid->ptr->NewSpanEvent(operation ? operation : ""));
        });
    });
}

pt_span_event_t pt_span_new_event_with_type(pt_span_t span, const char* operation,
                                            int32_t service_type) {
    return pt_api_call(__func__, static_cast<pt_span_event_t>(nullptr), [&] {
        return pt_handle_call(span, static_cast<pt_span_event_t>(nullptr),
                              [&](pt_span_t valid) {
            return make_span_event_handle(
                valid->ptr->NewSpanEvent(operation ? operation : "", service_type));
        });
    });
}

pt_span_event_t pt_span_get_event(pt_span_t span) {
    return pt_api_call(__func__, static_cast<pt_span_event_t>(nullptr), [&] {
        return pt_handle_call(span, static_cast<pt_span_event_t>(nullptr),
                              [](pt_span_t valid) {
            return make_span_event_handle(valid->ptr->GetSpanEvent());
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

size_t pt_span_get_trace_id(pt_span_t span, char* buf, size_t buf_size) {
    // Guarantee a valid empty C string on every early-return/error path (null
    // span, exception) where the lambda below never runs.
    if (buf && buf_size > 0) {
        buf[0] = '\0';
    }
    return pt_api_call(__func__, size_t{0}, [&] {
        return pt_handle_call(span, size_t{0}, [&](pt_span_t valid) {
            const std::string tid = valid->ptr->GetTraceId();
            if (buf && buf_size > 0) {
                // snprintf semantics: copy up to buf_size-1 bytes, always NUL-
                // terminate, and report the full length so the caller can detect
                // truncation (return >= buf_size).
                const size_t n = std::min(tid.size(), buf_size - 1);
                std::memcpy(buf, tid.data(), n);
                buf[n] = '\0';
            }
            return tid.size();
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

void pt_span_set_annotation_int(pt_span_t span, int32_t key, int32_t value) {
    pt_api_call(__func__, [&] {
        pt_handle_call(span, [&](pt_span_t valid) {
            valid->ptr->SetAnnotation(key, value);
        });
    });
}

void pt_span_set_annotation_long(pt_span_t span, int32_t key, int64_t value) {
    pt_api_call(__func__, [&] {
        pt_handle_call(span, [&](pt_span_t valid) {
            valid->ptr->SetAnnotation(key, value);
        });
    });
}

void pt_span_set_annotation_string(pt_span_t span, int32_t key, const char* value) {
    pt_api_call(__func__, [&] {
        if (!value) return;
        pt_handle_call(span, [&](pt_span_t valid) {
            valid->ptr->SetAnnotation(key, std::string_view(value));
        });
    });
}

void pt_span_set_annotation_string_string(pt_span_t span, int32_t key,
                                          const char* value1, const char* value2) {
    pt_api_call(__func__, [&] {
        pt_handle_call(span, [&](pt_span_t valid) {
            valid->ptr->SetAnnotation(key,
                                      value1 ? value1 : "",
                                      value2 ? value2 : "");
        });
    });
}

// ============================================================================
// SpanEvent operations
// ============================================================================

void pt_span_event_destroy(pt_span_event_t se) {
    // Span-event handles are non-owning pointer casts; there is nothing to
    // free. Kept as a safe no-op so existing callers remain valid.
    (void)se;
}

void pt_span_event_end(pt_span_event_t se) {
    pt_api_call(__func__, [&] {
        pt_handle_call(se, [](pinpoint::SpanEventPtr ev) {
            ev->EndEvent();
        });
    });
}

void pt_span_event_set_service_type(pt_span_event_t se, int32_t service_type) {
    pt_api_call(__func__, [&] {
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            ev->SetServiceType(service_type);
        });
    });
}

void pt_span_event_set_operation_name(pt_span_event_t se, const char* operation) {
    pt_api_call(__func__, [&] {
        if (!operation) return;
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            ev->SetOperationName(operation);
        });
    });
}

void pt_span_event_set_start_time_ms(pt_span_event_t se, int64_t ms_since_epoch) {
    pt_api_call(__func__, [&] {
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            ev->SetStartTime(ms_to_time_point(ms_since_epoch));
        });
    });
}

void pt_span_event_set_destination(pt_span_event_t se, const char* dest) {
    pt_api_call(__func__, [&] {
        if (!dest) return;
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            ev->SetDestination(dest);
        });
    });
}

void pt_span_event_set_end_point(pt_span_event_t se, const char* end_point) {
    pt_api_call(__func__, [&] {
        if (!end_point) return;
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            ev->SetEndPoint(end_point);
        });
    });
}

void pt_span_event_set_error(pt_span_event_t se, const char* error_message) {
    pt_api_call(__func__, [&] {
        if (!error_message) return;
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            ev->SetError(error_message);
        });
    });
}

void pt_span_event_set_error_named(pt_span_event_t se, const char* error_name,
                                   const char* error_message) {
    pt_api_call(__func__, [&] {
        if (!error_name || !error_message) return;
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            ev->SetError(error_name, error_message);
        });
    });
}

void pt_span_event_set_error_with_callstack(pt_span_event_t se,
                                            const char* error_name,
                                            const char* error_message,
                                            const pt_callstack_reader_t* reader) {
    pt_api_call(__func__, [&] {
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            const char* name = error_name    ? error_name    : "";
            const char* msg  = error_message ? error_message : "";
            if (reader) {
                CCallstackReader cpt_reader(reader);
                ev->SetError(name, msg, cpt_reader);
            } else {
                ev->SetError(name, msg);
            }
        });
    });
}

void pt_span_event_set_sql_query(pt_span_event_t se, const char* sql_query,
                                 const char* const* args, size_t args_count) {
    pt_api_call(__func__, [&] {
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            std::vector<pinpoint::SqlBindValue> sql_args;
            if (args) {
                sql_args.reserve(args_count);
                for (size_t i = 0; i < args_count; ++i) {
                    if (args[i]) {
                        sql_args.emplace_back(std::string_view{args[i]});
                    } else {
                        sql_args.emplace_back(nullptr);
                    }
                }
            }
            ev->SetSqlQuery(sql_query ? sql_query : "",
                            sql_args);
        });
    });
}

void pt_span_event_inject_context(pt_span_event_t se, pt_context_writer_t* writer) {
    pt_api_call(__func__, [&] {
        if (!writer) return;
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            CContextWriter cpt_writer(writer);
            ev->InjectContext(cpt_writer);
        });
    });
}

void pt_span_event_record_header(pt_span_event_t se, pt_header_type_t which,
                                 const pt_header_reader_t* reader) {
    pt_api_call(__func__, [&] {
        if (!reader) return;
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            CHeaderReader cpt_reader(reader);
            ev->RecordHeader(static_cast<pinpoint::HeaderType>(which), cpt_reader);
        });
    });
}

void pt_span_event_set_annotation_int(pt_span_event_t se, int32_t key, int32_t value) {
    pt_api_call(__func__, [&] {
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            ev->SetAnnotation(key, value);
        });
    });
}

void pt_span_event_set_annotation_long(pt_span_event_t se, int32_t key, int64_t value) {
    pt_api_call(__func__, [&] {
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            ev->SetAnnotation(key, value);
        });
    });
}

void pt_span_event_set_annotation_string(pt_span_event_t se, int32_t key,
                                         const char* value) {
    pt_api_call(__func__, [&] {
        if (!value) return;
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            ev->SetAnnotation(key, std::string_view(value));
        });
    });
}

void pt_span_event_set_annotation_string_string(pt_span_event_t se, int32_t key,
                                                const char* value1, const char* value2) {
    pt_api_call(__func__, [&] {
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            ev->SetAnnotation(key,
                              value1 ? value1 : "",
                              value2 ? value2 : "");
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
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            CHeaderReader cpt_req(request_reader);
            pinpoint::helper::TraceHttpClientRequest(ev,
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
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            CHeaderReader cpt_req(request_reader);
            CHeaderReader cpt_cookie(cookie_reader);
            pinpoint::helper::TraceHttpClientRequest(ev,
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
        pt_handle_call(se, [&](pinpoint::SpanEventPtr ev) {
            CHeaderReader cpt_resp(response_reader);
            pinpoint::helper::TraceHttpClientResponse(ev, status_code, cpt_resp);
        });
    });
}
