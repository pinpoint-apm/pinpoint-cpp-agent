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

#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

#include "pinpoint/tracer.h"
#include "active_span.h"
#include "agent_runtime.h"
#include "agent_service.h"
#include "url_stat.h"

namespace pinpoint {

    /// @brief Returns a shared noop span event instance.
    SpanEventPtr noopSpanEvent();
    /**
     * @brief Returns a shared noop span instance.
     *
     * Always the same `NoopSpan` object (`.get()` is stable process-wide), but
     * handed out through a per-thread control block so the returned copy's
     * refcount traffic stays on the calling thread — see the definition.
     *
     * Cannot throw once the calling thread has been set up, which is what the
     * CATCH_AND_LOG_RETURN handlers returning this rely on. Setting the thread
     * up is best-effort and degrades to the shared holder on failure, leaving
     * only the process-wide singleton's own first-call allocation as a throwing
     * path — unchanged from before the per-thread owner existed.
     */
    SpanPtr noopSpan();
    /// @brief Returns the global singleton span event handed out by unsampled spans.
    SpanEventPtr unsampledSpanEvent();
    /// @brief Returns a shared noop agent instance.
    AgentPtr noopAgent();

    /// @brief Span event implementation that ignores all recording operations.
    class NoopSpanEvent : public SpanEvent {
    public:
        void SetServiceType(int32_t type) override {}
        void SetOperationName(std::string_view operation) override {}
        void SetStartTime(std::chrono::system_clock::time_point start_time) override {}
        void SetDestination(std::string_view dest) override {}
        void SetEndPoint(std::string_view end_point) override {}
        void SetError(std::string_view error_message) override {}
        void SetError(std::string_view error_name, std::string_view error_message) override {}
        void SetError(std::string_view error_name, std::string_view error_message, CallStackReader& reader) override {}
        void SetError(std::string_view error_name, std::string_view error_message,
                      const std::vector<CallStackFrame>& frames) override {}
        void SetSqlQuery(std::string_view sql_query,
                         const std::vector<SqlBindValue>& bind_args) override {}
        void RecordHeader(HeaderType which, HeaderReader& reader) override {}
        void InjectContext(TraceContextWriter& writer) override {}
        void SetNextSpanId(int64_t next_span_id) override {}

        void SetAnnotation(int32_t key, int32_t value) override {}
        void SetAnnotation(int32_t key, int64_t value) override {}
        void SetAnnotation(int32_t key, std::string_view value) override {}
        void SetAnnotation(int32_t key,
                           std::string_view value1,
                           std::string_view value2) override {}
        void EndEvent() override {}
    };

    /**
     * @brief Span event handed out by UnsampledSpan: records nothing but still
     *        propagates the unsampled decision (`Pinpoint-Sampled: s0`) to
     *        downstream services on InjectContext.
     *
     * Stateless, so a single global instance (see unsampledSpanEvent()) is
     * shared by every unsampled span.
     */
    class UnsampledSpanEvent final : public NoopSpanEvent {
    public:
        void InjectContext(TraceContextWriter& writer) override;
    };

    /// @brief Span implementation used when tracing is disabled.
    class NoopSpan : public Span {
    public:
        SpanEventPtr NewSpanEvent(std::string_view operation) override { return noopSpanEvent(); }
        SpanEventPtr NewSpanEvent(std::string_view operation, int32_t service_type) override { return noopSpanEvent(); }
        SpanEventPtr GetSpanEvent() override { return noopSpanEvent(); }
        void EndSpan() override {}
        SpanPtr NewAsyncSpan(std::string_view async_operation) override { return noopSpan(); }
        SpanPtr NewAsyncSpan(std::string_view async_operation,
                             int32_t async_id, int32_t async_sequence) override { return noopSpan(); }
        SpanEventPtr RecordSpanEvent(std::string_view operation, int32_t service_type,
                                     int32_t sequence, int32_t depth,
                                     int64_t start_time_ms, int64_t end_time_ms,
                                     int32_t async_id) override { return noopSpanEvent(); }

        void SetServiceType(int32_t service_type) override {}
        void SetStartTime(std::chrono::system_clock::time_point start_time) override {}
        void SetRemoteAddress(std::string_view address) override {}
        void SetEndPoint(std::string_view end_point) override {}
        void SetAcceptorHost(std::string_view host) override {}
        void SetError(std::string_view error_message) override {}
        void SetError(std::string_view error_name, std::string_view error_message) override {}
        void SetStatusCode(int status) override {}
        void SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) override {}
        void SetLogging(TraceContextWriter& writer) override {}
        void RecordHeader(HeaderType which, HeaderReader& reader) override {}
        void SetAnnotation(int32_t key, int32_t value) override {}
        void SetAnnotation(int32_t key, int64_t value) override {}
        void SetAnnotation(int32_t key, std::string_view value) override {}
        void SetAnnotation(int32_t key,
                           std::string_view value1,
                           std::string_view value2) override {}
        void SetAnnotation(int32_t key, int64_t long_value,
                           int32_t int_value1, int32_t int_value2,
                           int32_t byte_value1, int32_t byte_value2,
                           std::string_view string_value) override {}

        std::string GetTraceId() override { return {}; }
        int64_t GetSpanId() override { return 0; }
        bool IsSampled() override { return false; }
    };

    /// @brief Lightweight span used when requests are explicitly marked as unsampled.
    class UnsampledSpan final : public NoopSpan {
    public:
        // `runtime` is the creator's already-loaded runtime snapshot (see
        // SpanImpl). With it, SetUrlStat gates on the snapshot's config and
        // EndSpan resolves the failure status without any atomic runtime
        // load. When omitted (tests), both fall back to the agent calls.
        explicit UnsampledSpan(AgentService *agent,
                               std::shared_ptr<const AgentRuntime> runtime = nullptr);
        ~UnsampledSpan() override;

        // Hand out the unsampled span event so that InjectContext on the event
        // still propagates the `s0` sampling decision downstream.
        SpanEventPtr NewSpanEvent(std::string_view operation) override { return unsampledSpanEvent(); }
        SpanEventPtr NewSpanEvent(std::string_view operation, int32_t service_type) override { return unsampledSpanEvent(); }
        SpanEventPtr GetSpanEvent() override { return unsampledSpanEvent(); }

        void EndSpan() override;
        void SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) override;

        int64_t GetSpanId() override {
            return span_id_;
        }

    private:
        // Shared by EndSpan's catch handlers; see the definition.
        void releaseActiveSpanOnError() noexcept;
        // The stats sink this span records into: the runtime snapshot's
        // shared AgentStats when the span was admitted with one (production),
        // else the agent's own (tests), else null (no agent at all).
        AgentStats* statsSink() const;

        int64_t span_id_;
        int64_t start_time_;
        std::atomic<bool> finished_{false};
        // Registration in the active-request registry (see active_span.h):
        // linked by the constructor, unlinked by EndSpan; the destructor and
        // releaseActiveSpanOnError unlink as idempotent backstops.
        ActiveSpanNode active_node_;
        // Guards url_stat_ between SetUrlStat() and EndSpan(): unlike the
        // fully-stateful SpanImpl, this span's only mutable state is this one
        // optional, so a mutex is cheap insurance that turns the
        // contract-violating concurrent SetUrlStat/EndSpan into a defined
        // warn/no-op instead of a data race on the moved-from entry.
        std::mutex url_stat_mutex_;
        std::optional<UrlStatEntry> url_stat_;
        // Runtime snapshot of this span's admission decision; null only when
        // constructed without one (tests). See the ctor comment.
        std::shared_ptr<const AgentRuntime> runtime_;
        // Agent keep-alive, held ONLY when runtime_ does not carry the stats
        // sinks (tests, hand-built runtimes). A production span reaches
        // everything it touches after construction through runtime_, so it
        // skips the selfRef() this used to cost — a per-request CAS on the
        // agent's single control block; see the ctor.
        std::shared_ptr<AgentService> agent_ref_;
        // Dereferenced during construction (caller holds the agent) and on
        // the agent_ref_-protected fallback paths only: without the
        // keep-alive this pointer may dangle once the host releases the
        // agent, and EndSpan/the destructor must not touch it.
        AgentService *agent_;
    };

    /// @brief Agent implementation that always returns noop spans.
    class NoopAgent final : public Agent {
    public:
        SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point) override { return noopSpan(); }
        SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point,
            TraceContextReader& reader) override { return noopSpan(); }
        SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point, std::string_view method,
            TraceContextReader& reader) override { return noopSpan(); }
        SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point, std::string_view method,
            const std::map<std::string, std::string>& pinpoint_headers) override { return noopSpan(); }

        bool Enable() override { return false; }
        void Shutdown() override {}
    };

    /// @brief Trace context reader that never returns any context information.
    class NoopTraceContextReader final : public TraceContextReader {
    public:
        std::optional<std::string_view> Get(std::string_view key) const override { return std::nullopt; }
    };

}
