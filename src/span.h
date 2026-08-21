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

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "active_span.h"
#include "agent_runtime.h"
#include "agent_service.h"
#include "callstack.h"
#include "config.h"
#include "span_event.h"
#include "url_stat.h"
#include "utility.h"

namespace pinpoint {
    
    constexpr int SPAN_FLAG_NONE = 0;
    constexpr int SPAN_ERR_NONE = 0;
    constexpr int32_t SPAN_LOGGING_FLAG_OFF = 0;
    constexpr int32_t SPAN_LOGGING_FLAG_ON = 1;

    /**
     * @brief Holds mutable span state until it is converted into the
     *        `SpanChunk` messages destined for the collector.
     */
    class SpanData final {
    public:
        SpanData(std::string_view operation, int32_t app_type, int32_t api_id);
        ~SpanData() = default;

        TraceId& getTraceId() { return trace_id_; }
        // By value so an rvalue moves straight through without copying the
        // AgentId string a second time.
        void setTraceId(TraceId trace_id) {
            trace_id_ = std::move(trace_id);
            trace_id_wire_.clear();
        }
        /// @brief Wire form (`agentId^startTime^sequence`), built once and
        /// cached: immutable for the span's lifetime, while injectContext()
        /// resends it on every outbound call. Span-owning thread only; the gRPC
        /// workers read trace_id_ directly.
        const std::string& getTraceIdWire() {
            if (trace_id_wire_.empty() && !trace_id_.empty()) {
                trace_id_wire_ = trace_id_.toString();
            }
            return trace_id_wire_;
        }

        void setSpanId(int64_t span_id) { span_id_ = span_id; }
        int64_t getSpanId() const { return span_id_; }

        int32_t getAppType() const { return app_type_; }
        /// @brief Operation name, kept only as the api-id fallback.
        ///
        /// Empty whenever getApiId() is positive: the id identifies the
        /// operation on the wire, so the name is not stored a second time.
        /// Only build_grpc_span's no-api-id branch consumes this.
        const std::string& getOperationName() const { return operation_; }
        int32_t getApiId() const { return api_id_; }

        void setParentSpanId(int64_t parent_span_id) { parent_span_id_ = parent_span_id; }
        int64_t getParentSpanId() const { return parent_span_id_; }

        void setParentAppType(int parent_app_type) { parent_app_type_ = parent_app_type; }
        int32_t getParentAppType() const { return parent_app_type_; }

        void setParentAppName(std::string_view parent_app_name) { parent_app_name_ = parent_app_name; }
        std::string& getParentAppName() { return parent_app_name_; }

        void setParentServiceName(std::string_view parent_service_name) { parent_service_name_ = parent_service_name; }
        std::string& getParentServiceName() { return parent_service_name_; }

        void setServiceType(int service_type) { service_type_ = service_type; }
        int32_t getServiceType() const { return service_type_; }

        void setRpcName(std::string_view rpc_name) { rpc_name_ = rpc_name; }
        std::string& getRpcName() { return rpc_name_; }

        // Endpoint and remote address both DEFAULT to the acceptor host: an
        // inbound Pinpoint-Host names the host the caller dialed, and that is
        // the best answer for all three until instrumentation supplies better.
        // Applying the default in the getters means only the acceptor host is
        // stored, instead of three allocations of identical bytes per
        // continued trace. The wire is unchanged: a field nobody sets still
        // serializes as the host (see build_accept_event).
        //
        // The explicit `*_set_` flags rather than an empty() check: setting a
        // field to "" on purpose must keep meaning "empty", not silently
        // resurrect the host default.

        void setEndPoint(std::string_view endpoint) {
            endpoint_ = endpoint;
            endpoint_set_ = true;
        }
        const std::string& getEndPoint() const { return endpoint_set_ ? endpoint_ : acceptor_host_; }

        void setRemoteAddr(std::string_view remote_addr) {
            remote_addr_ = remote_addr;
            remote_addr_set_ = true;
        }
        const std::string& getRemoteAddr() const { return remote_addr_set_ ? remote_addr_ : acceptor_host_; }

        void setAcceptorHost(std::string_view acceptor_host) { acceptor_host_ = acceptor_host; }
        const std::string& getAcceptorHost() const { return acceptor_host_; }

        void setLoggingFlag() { logging_flag_ = SPAN_LOGGING_FLAG_ON; }
        int32_t getLoggingFlag() const { return logging_flag_; }

        void setFlags(int flags) { flags_ = flags; }
        int getFlags() const { return flags_; }

        int32_t getEventSequence() const { return event_sequence_.load(std::memory_order_relaxed); }
        int32_t getEventDepth() const { return event_depth_.load(std::memory_order_relaxed); }
        /// @brief Reserves the next event sequence and depth for a new span event.
        std::pair<int32_t, int32_t> nextEventSequenceAndDepth() {
            return {
                event_sequence_.fetch_add(1, std::memory_order_relaxed),
                event_depth_.fetch_add(1, std::memory_order_relaxed)
            };
        }
        void decrEventDepth() { event_depth_.fetch_sub(1, std::memory_order_relaxed); }

        void setErr(int err) { err_ = err; }
        int getErr() const { return err_; }

        void setErrorFuncId(int32_t error_func_id) { error_func_id_ = error_func_id; }
        int32_t getErrorFuncId() const { return error_func_id_; }

        void setErrorString(std::string_view error_string) { error_string_ = error_string; }
        const std::string& getErrorString() const { return error_string_; }

        void setAsyncId(int32_t async_id) { async_id_ = async_id; }
        int32_t getAsyncId() const { return async_id_; }
        bool isAsyncSpan() const { return async_id_ != NONE_ASYNC_ID; }

        void setAsyncSequence(int32_t async_seq) { async_sequence_ = async_seq; }
        int32_t getAsyncSequence() const { return async_sequence_; }

        /// @brief Stores the start timestamp in epoch milliseconds.
        void setStartTime(std::chrono::system_clock::time_point start_time) { start_time_ = to_milli_seconds(start_time); }
        int64_t getStartTime() const { return start_time_; }

        /// @brief Captures the end time and computes the elapsed duration.
        void setEndTime() {
            end_time_ = std::chrono::system_clock::now();
            // system_clock can step backwards (NTP); never report a negative
            // elapsed time. Only the low side is clamped: the wire field is
            // int32 ms, so a delta beyond INT32_MAX ms (~24.8 days — e.g. a
            // user-supplied start time in seconds instead of ms) wraps.
            elapsed_ = static_cast<int32_t>(std::max<int64_t>(to_milli_seconds(end_time_) - start_time_, 0));
        }
        std::chrono::system_clock::time_point getEndTime() const { return end_time_; }
        int32_t getElapsed() const { return elapsed_; }

        /// @brief Pushes a newly created span event onto the event stack.
        SpanEventImpl* addSpanEvent(std::unique_ptr<SpanEventImpl> se);
        /**
         * @brief Finalizes span events down to and including `expected`.
         *
         * A well-nested trace has `expected` on top, so this is a single pop.
         * When user code ends events out of order (parent before child), the
         * events above `expected` are implicitly finished — as EndSpan does —
         * rather than finishing a different event with `expected`'s end time
         * and stranding `expected` on the stack. Logged so misuse is visible.
         */
        void finishSpanEvent(SpanEventImpl* expected);
        /**
         * @brief Finalizes every span event still on the stack (LIFO order).
         *
         * Called at EndSpan so events user code failed to end — plus the async
         * root event, which stays open until EndSpan by design — reach the
         * final chunk instead of being dropped.
         *
         * @return Number of events that were still open.
         */
        size_t finishOpenSpanEvents();
        SpanEventImpl* topSpanEvent() {
            return event_stack_.empty() ? nullptr : event_stack_.back().get();
        }

        /**
         * @brief Drains finished span events into a chunk view.
         *
         * Ownership stays with this SpanData: events move to the retired list
         * and `out` receives borrowed pointers valid for this SpanData's
         * lifetime. That is what keeps a user-held raw SpanEventPtr safe after
         * a mid-span chunk flush — the duplicate-EndEvent no-op documented in
         * pinpoint/tracer.h must never touch freed memory.
         */
        void takeFinishedEvents(std::vector<SpanEventImpl*>& out);
        size_t getFinishedEventsCount() const {
            return finished_events.size();
        }

        // Owned by value; the list inside only allocates on the first append.
        PinpointAnnotation* getAnnotations() { return &annotations_; }

    private:
        void storeFinishedEvent(std::unique_ptr<SpanEventImpl> se);

        TraceId trace_id_;
        // Lazily-built toString() cache; see getTraceIdWire().
        std::string trace_id_wire_;
        int64_t span_id_{};

        int64_t parent_span_id_{-1};
        std::string parent_app_name_;
        int32_t parent_app_type_{1};
        std::string parent_service_name_;

        int32_t app_type_;
        int32_t service_type_{defaults::SPAN_SERVICE_TYPE};
        std::string operation_;
        int32_t api_id_;

        std::string rpc_name_;
        // Plain bools, not atomics, for the same reason the strings beside them
        // are plain: the owning thread writes them, and the gRPC worker reads
        // the endpoint only through SpanChunk's own snapshot until the span is
        // finished (see SpanChunk::endpoint_).
        std::string endpoint_;
        bool endpoint_set_{false};
        std::string remote_addr_;
        bool remote_addr_set_{false};
        std::string acceptor_host_;

        // Atomic so overflow checks and event position reservation never race
        // concurrent NewSpanEvent calls.
        std::atomic<int32_t> event_sequence_{0};
        std::atomic<int32_t> event_depth_{1};

        int32_t logging_flag_{SPAN_LOGGING_FLAG_OFF};
        int flags_{SPAN_FLAG_NONE};
        int err_{SPAN_ERR_NONE};
        int32_t error_func_id_{};
        std::string error_string_;

        int64_t start_time_;
        std::chrono::system_clock::time_point end_time_;
        int32_t elapsed_{};

        int32_t async_id_{NONE_ASYNC_ID};
        int32_t async_sequence_{};

        /// @brief Pops the most recent open span event, or nullptr if none is
        /// open. Owning thread only.
        std::unique_ptr<SpanEventImpl> popSpanEvent() {
            if (event_stack_.empty()) {
                return nullptr;
            }
            auto se = std::move(event_stack_.back());
            event_stack_.pop_back();
            return se;
        }

        // LIFO stack of the open (nested) span events, back() being the most
        // recent. A vector, not a deque (nor std::stack's deque backing): on
        // libstdc++ a default-constructed deque allocates its iterator map plus
        // a 512-byte block in every span's constructor even when no event is
        // ever recorded, while a vector allocates nothing. Nesting depth is
        // capped by span.max_event_depth, so regrowth is rare.
        std::vector<std::unique_ptr<SpanEventImpl>> event_stack_;
        // Kept sequence-ordered as events finish so chunks do not need to sort.
        // Not mutex-guarded: a span is single-threaded (see the Span
        // thread-safety contract in pinpoint/tracer.h). A vector for the same
        // reason as event_stack_, and it keeps its capacity across chunk
        // flushes (takeFinishedEvents clears it).
        //
        // Which insert path storeFinishedEvent takes follows from the trace
        // shape, not from misuse: siblings finish in creation order and append,
        // nested events finish innermost-first and insert at the front. Either
        // way the move stays small — the list drains into a chunk every
        // span.event_chunk_size finishes, and the one burst that can exceed
        // that (EndSpan draining the open stack) is capped by max_event_depth.
        std::vector<std::unique_ptr<SpanEventImpl>> finished_events;
        // Finished events already handed to a chunk. Ownership is retained here
        // so raw SpanEventPtr handles held by user code stay valid until the
        // span data is released: a late duplicate EndEvent after a chunk flush
        // lands on a live object and stays the no-op the public headers
        // promise. Only tombstones are retained long-term — ~SpanChunk releases
        // each event's owned heap (strings, annotations) once the chunk is done
        // with it, so a long-lived span grows by a fixed-size husk per event.
        std::vector<std::unique_ptr<SpanEventImpl>> retired_events_;

        // Owned by value: a span that records no annotation pays no heap.
        PinpointAnnotation annotations_;
    };

    /// @brief Represents a batch of span events emitted as a single gRPC message.
    class SpanChunk final {
    public:
        SpanChunk(const std::shared_ptr<SpanData>& span_data, bool final);
        /// @brief Releases the retired events' heavy payload; see span.cpp.
        ~SpanChunk();

        // Non-copyable (which also suppresses the implicit moves): the
        // destructor releases the events' payload, so a copy would let the
        // first-destroyed sibling wipe it out from under the other, silently
        // emptying whatever it still had to serialize. Chunks are only ever
        // handled as std::unique_ptr<SpanChunk>; keep it that way.
        SpanChunk(const SpanChunk&) = delete;
        SpanChunk& operator=(const SpanChunk&) = delete;

        /// @brief Compacts the span event list by removing completed events.
        void optimizeSpanEvents();

        std::shared_ptr<SpanData>& getSpanData() { return span_data_; }
        std::vector<SpanEventImpl*>& getSpanEventChunk() { return event_chunk_; }
        /// @brief Timestamp used for ordering span chunks.
        int64_t getKeyTime() const { return key_time_; }
        bool isFinal() const { return final_; }
        const std::string& getEndPoint() const { return endpoint_; }

    private:
        std::shared_ptr<SpanData> span_data_;
        // Borrowed from span_data_'s retired list; kept alive by span_data_.
        std::vector<SpanEventImpl*> event_chunk_;
        bool final_;
        int64_t key_time_;
        // Copied from span_data_ at construction (on the span-owning thread).
        // A non-final chunk is serialized on the gRPC span worker while the
        // span is still live, and SetEndPoint() is the one mutator that stays
        // legal in that window — the worker must read this snapshot, never
        // span_data_'s endpoint_, or it races the owning thread's write to a
        // plain std::string.
        std::string endpoint_;
    };

    /**
     * @brief Concrete span implementation used when tracing is enabled.
     *
     * Delegates storage to `SpanData` while coordinating span event creation,
     * context propagation and final submission through the agent service.
     *
     * @warning Single-threaded per instance. See the `Span` thread-safety
     *          contract in pinpoint/tracer.h: one `SpanImpl` (and the
     *          `SpanEventImpl`s it returns as raw `SpanEventPtr`) must be used
     *          by a single thread for its whole lifetime. Concurrent calls are
     *          undefined behaviour and can crash — `exceptions_`, `url_stat_`
     *          and the `SpanData` string/annotation buffers are unsynchronized,
     *          and releasing the span on one thread frees span events still
     *          referenced through raw pointers on another. The
     *          `finished_`/`overflow_` atomics are idempotency guards (a
     *          repeated EndSpan is a no-op), NOT a concurrency guarantee.
     */
    class SpanImpl final : public Span {
    public:
        // `runtime` is the creator's already-loaded snapshot (the AgentRuntime
        // generation NewSpan sampled against). Passing it skips every further
        // atomic runtime load and keeps the span on the exact generation of its
        // admission decision. When omitted (tests), the ctor loads the config
        // itself and status-error checks fall back to agent->isStatusFail().
        SpanImpl(AgentService* agent, std::string_view operation, std::string_view rpc_point,
                 std::shared_ptr<const AgentRuntime> runtime = nullptr);
        ~SpanImpl() override;

        SpanEventPtr NewSpanEvent(std::string_view operation) override {
            return NewSpanEvent(operation, defaults::SPAN_EVENT_SERVICE_TYPE);
        }
        SpanEventPtr NewSpanEvent(std::string_view operation, int32_t service_type) override;
        SpanEventPtr GetSpanEvent() override;
        /// @brief Finalizes the span and queues it for asynchronous delivery.
        void EndSpan() override;
        SpanPtr NewAsyncSpan(std::string_view async_operation) override;
        SpanPtr NewAsyncSpan(std::string_view async_operation,
                             int32_t async_id, int32_t async_sequence) override;
        SpanEventPtr RecordSpanEvent(std::string_view operation, int32_t service_type,
                                     int32_t sequence, int32_t depth,
                                     int64_t start_time_ms, int64_t end_time_ms,
                                     int32_t async_id) override;

        /**
         * @brief Extracts a span context from an inbound propagation carrier.
         *
         * Impl-level only: called by AgentImpl::NewSpan right after creation.
         *
         * @param trace_id The already-resolved trace id — parsed from the
         *                 inbound HEADER_TRACE_ID or generated by NewSpan. Must
         *                 be non-empty (NewSpan turns an empty/failed trace id
         *                 into a noop span). Moved into the span's SpanData.
         */
        void extractContext(TraceContextReader& reader, TraceId trace_id);
        /**
         * @brief Injects this span's context into an outbound carrier.
         *
         * Impl-level only: called by the span events this span hands out
         * (SpanEventImpl and DisabledSpanEvent) to implement
         * SpanEvent::InjectContext. No-op once the span is finished.
         */
        void injectContext(TraceContextWriter& writer, int64_t next_span_id, std::string_view host);
        /**
         * @brief Pops and finalizes span events down to `se` (top of stack when
         * the trace is well nested; see SpanData::finishSpanEvent).
         *
         * Impl-level only: called by SpanEventImpl::EndEvent, which guards
         * against duplicate ends before delegating here.
         */
        void endSpanEvent(SpanEventImpl* se);
        /**
         * @brief Consumes one pending overflow placeholder.
         *
         * Impl-level only: called by DisabledSpanEvent::EndEvent. Overflowed
         * events are never pushed onto the stack, so ending one must not pop a
         * real event; it only balances NewSpanEvent's overflow counter. Warns
         * when there is no pending overflow (duplicate end).
         */
        void endDisabledSpanEvent();

        // Out-of-line: getTraceIdWire() lazily builds (and the return copies)
        // a string, so this needs the exception boundary in span.cpp.
        std::string GetTraceId() override;
        int64_t GetSpanId() override { return data_->getSpanId(); }
        bool IsSampled() override { return true; }
        SpanConfigSnapshot GetConfigSnapshot() const override;
        // Cheap per-creation read for binding layers: just the revision of the
        // config generation this span captured, no snapshot building.
        int64_t GetConfigRevision() const override {
            return config_ ? config_->revision : 0;
        }
        // Annotation overloads are out-of-line because string payload copies
        // and list growth must stay inside the exception boundary in span.cpp.
        // A finished span is a warning no-op.
        void SetAnnotation(int32_t key, int32_t value) override;
        void SetAnnotation(int32_t key, int64_t value) override;
        void SetAnnotation(int32_t key, std::string_view value) override;
        void SetAnnotation(int32_t key,
                           std::string_view value1,
                           std::string_view value2) override;
        void SetAnnotation(int32_t key, int64_t long_value,
                           int32_t int_value1, int32_t int_value2,
                           int32_t byte_value1, int32_t byte_value2,
                           std::string_view string_value) override;
        const std::shared_ptr<SpanData>& getSpanData() const { return data_; }
        const std::vector<std::unique_ptr<Exception>>& getExceptions() const { return exceptions_; }
        std::vector<std::unique_ptr<Exception>> takeExceptions() { return std::move(exceptions_); }
        std::string getUrlTemplate() const {
            if (url_stat_) {
                return url_stat_->url_pattern_;
            }
            return "NULL";
        }

        void SetServiceType(int32_t service_type) override;
        void SetStartTime(std::chrono::system_clock::time_point start_time) override;
        void SetRemoteAddress(std::string_view address) override;
        void SetEndPoint(std::string_view end_point) override;
        void SetAcceptorHost(std::string_view host) override;
        void SetError(std::string_view error_message) override;
        void SetError(std::string_view error_name, std::string_view error_message) override;
        void SetStatusCode(int status) override;
        void SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) override;
        /// @brief Records the logging flag and injects the context into a logger.
        void SetLogging(TraceContextWriter& writer) override;
        void RecordHeader(HeaderType which, HeaderReader& reader) override;

    private:
        friend class SpanEventImpl;

            // Keeps the agent alive while user code still holds this span (as
            // UnsampledSpan::agent_ref_ does): a span may legally outlive
            // Shutdown(), and EndSpan and the recorders dereference agent_.
            // Null only for agents that are not shared_ptr-owned (tests).
            std::shared_ptr<AgentService> agent_ref_;
            AgentService *agent_;
            // Runtime snapshot taken once at span creation (see the ctor).
            // Null only for spans constructed without one (tests), which fall
            // back to the agent call.
            std::shared_ptr<const AgentRuntime> runtime_;
            // Alias for runtime_->config (or the ctor-loaded config when
            // runtime_ is null — never null itself). The per-event hot paths
            // read this instead of agent_->getConfig(), so they pay no atomic
            // shared_ptr load per call and the span's limits stay consistent
            // even when a config reload lands mid-span.
            std::shared_ptr<const Config> config_;
            std::shared_ptr<SpanData> data_;
            std::atomic<int32_t> overflow_;
            std::atomic<bool> finished_;
            // Registration in the active-request registry (see active_span.h):
            // linked by extractContext, unlinked by EndSpan; the destructor and
            // releaseActiveSpanOnError unlink as idempotent backstops. Async
            // spans never link theirs.
            ActiveSpanNode active_node_;
            std::optional<UrlStatEntry> url_stat_;
            std::vector<std::unique_ptr<Exception>> exceptions_;
            // Handed out instead of a real event while the event stack is
            // overflowed (Java agent's DisableSpanEvent parity): records
            // nothing but still injects full trace context. Created lazily on
            // the first overflow, one shared instance per span.
            std::unique_ptr<DisabledSpanEvent> disabled_event_;
            SpanEventPtr disabledSpanEvent();

            // Owning-thread guard enforcing the Span single-thread contract
            // (see pinpoint/tracer.h). Bound lazily on the first NewSpanEvent
            // rather than at construction, so an async child created on one
            // thread but consumed on another binds to its consuming thread.
            // A relaxed load-and-compare once bound (only the binding CASes),
            // cheap enough to stay on in release builds: a violation logs
            // there and asserts in debug. Atomic so the detection path — which
            // by definition runs under concurrent use — is not itself a race.
            std::atomic<std::thread::id> owner_thread_id_{};
            void checkOwnerThread();

            /// @brief Emits a span chunk via the agent service.
            void record_chunk(bool final) const;
            // Shared by EndSpan's catch handlers; see the definition.
            void releaseActiveSpanOnError() noexcept;
            void sendUrlStat();
            void sendExceptions();
            // Snapshot-based equivalent of agent_->isStatusFail(), with no
            // atomic runtime load. Falls back to the agent call for spans
            // constructed without a snapshot (tests).
            bool isStatusFail(int status) const;
            // Exceptions only drain at EndSpan (unlike span events, which
            // chunk-flush mid-span), so a retry loop on a long-lived span
            // would grow this without bound — each entry carries a full
            // string callstack. Excess exceptions are dropped.
            static constexpr size_t kMaxBufferedExceptions = 100;
            // Returns false when the exception was dropped, so the caller can
            // skip the ANNOTATION_EXCEPTION_ID that would reference an id
            // never sent.
            bool addException(std::unique_ptr<Exception> exception) {
                if (exceptions_.size() >= kMaxBufferedExceptions) {
                    return false;
                }
                exceptions_.push_back(std::move(exception));
                return true;
            }
    };

}  // namespace pinpoint
