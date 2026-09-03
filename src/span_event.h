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
#include <memory>

#include "pinpoint/tracer.h"
#include "annotation.h"
#include "utility.h"

namespace pinpoint {

    class AgentService;
    class CallStack;
    class SpanData;
    class SpanImpl;

    /// @brief Concrete span event implementation that records timing and metadata.
    class SpanEventImpl final : public SpanEvent {
    public:
        SpanEventImpl(SpanImpl* span, std::string_view operation);
        ~SpanEventImpl() override {}

        void SetServiceType(int32_t type) override { if (warnIfFinished()) return; service_type_ = type; }
        // Out-of-line: the string assignment allocates, so it needs the
        // exception boundary in span_event.cpp (as do the allocating setters
        // below).
        void SetOperationName(std::string_view operationName) override;
        void SetStartTime(std::chrono::system_clock::time_point start_time) override { if (warnIfFinished()) return; start_time_ = to_milli_seconds(start_time); }
        void SetDestination(std::string_view dest) override;
        void SetEndPoint(std::string_view endpoint) override;
        void SetError(std::string_view error_message) override;
        void SetError(std::string_view error_name, std::string_view error_message) override;
        void SetError(std::string_view error_name, std::string_view error_message, CallStackReader& reader) override;
        void SetError(std::string_view error_name, std::string_view error_message,
                      const std::vector<CallStackFrame>& frames) override;
        void SetSqlQuery(std::string_view sql_query,
                         const std::vector<SqlBindValue>& bind_args) override;
        void RecordHeader(HeaderType which, HeaderReader& reader) override;
        /// @brief Writes this event's outbound context (trace id, generated
        /// child span id, parent app info) into a propagation carrier.
        void InjectContext(TraceContextWriter& writer) override;
        void SetNextSpanId(int64_t next_span_id) override;
        // Out-of-line: string copies and list growth must stay inside the
        // exception boundary in span_event.cpp. A finished event no-ops.
        void SetAnnotation(int32_t key, int32_t value) override;
        void SetAnnotation(int32_t key, int64_t value) override;
        void SetAnnotation(int32_t key, std::string_view value) override;
        void SetAnnotation(int32_t key,
                           std::string_view value1,
                           std::string_view value2) override;
        /// @brief Finalizes this event through the parent span. Guarded so a
        /// duplicate call is a warning no-op instead of popping (and thereby
        /// corrupting) another event from the span's event stack.
        void EndEvent() override;

        /// @brief Finalizes the span event by computing elapsed metrics.
        void finish();

        /**
         * @brief Releases the heap this retired event owns (strings and the
         * annotation list), leaving a small tombstone at a stable address.
         *
         * Called by ~SpanChunk once the chunk is done with the event — the
         * gRPC builders copied every field into arena protobuf, or the chunk
         * was dropped unsent — so nothing reads the payload afterwards: every
         * accessor and mutator has been a finished_-guarded no-op since
         * finish(), which is also why freeing from the gRPC worker thread
         * cannot race user-held raw SpanEventPtr handles. Only the payload
         * dies here; the object must stay alive at its address until the
         * owning SpanData does (see SpanData::retired_events_).
         */
        void releaseRetiredPayload() noexcept;

        int32_t getServiceType() const { return service_type_; }
        /// @brief Operation name, kept only as the api-id fallback.
        ///
        /// Empty whenever getApiId() is positive — the id identifies the
        /// operation on the wire — unless SetOperationName() supplied one
        /// explicitly. Only build_span_event's no-api-id branch consumes this.
        const std::string& getOperationName() const { return operation_; }

        int64_t getStartTime() const { return start_time_; }
        /// @brief Overrides the start timestamp (epoch ms) for batch-replayed
        /// events (Span::RecordSpanEvent), whose timing is caller-recorded.
        void setStartTime(int64_t start_time_ms) { start_time_ = start_time_ms; }
        /// @brief Presets the end timestamp (epoch ms): finish() then computes
        /// elapsed from it instead of the wall clock. 0 = use the wall clock.
        void setEndTime(int64_t end_time_ms) { end_time_ = end_time_ms; }
        /// @brief Start offset relative to the parent span.
        void setStartElapsed(int32_t elapsed) { start_elapsed_ = elapsed; }
        int32_t getStartElapsed() const { return start_elapsed_; }
        int32_t getEndElapsed() const { return elapsed_; }

        void setSequence(int32_t sequence) { sequence_ = sequence; }
        int32_t getSequence() const { return sequence_; }
        void setDepth(int32_t depth) { depth_ = depth; }
        int32_t getDepth() const { return depth_; }

        /// @brief Generates the next span identifier for asynchronous spans.
        int64_t generateNextSpanId();
        int64_t getNextSpanId() const { return next_span_id_; }

        // Owned by value; the list inside only allocates on the first append.
        PinpointAnnotation* getAnnotations() { return &annotations_; }

        std::string& getEndPoint() { return endpoint_; }
        std::string& getDestinationId() { return destination_id_; }

        int32_t getErrorFuncId() const { return error_func_id_; }
        std::string& getErrorString() { return error_string_; }

        void setAsyncId(const int32_t async_id) { async_id_ = async_id; }
        int32_t getAsyncId() const { return async_id_; }

        void incrAsyncSeq() { async_seq_gen_++; }
        int32_t getAsyncSeqGen() const { return async_seq_gen_; }

        void setApiId(int32_t api_id) { api_id_ = api_id; }
        int32_t getApiId() const { return api_id_; }

    private:
        /// @brief Shared tail of the callstack SetError overloads: wrap the
        /// built call stack in an Exception on the parent span and stamp the
        /// exception-id annotation. `span` comes from the caller's
        /// spanIfAlive() check, so it is never the dead-span case.
        void recordException(SpanImpl& span, std::unique_ptr<CallStack> callstack);

        /**
         * @brief The parent span while it is alive, else nullptr (after a
         *        warning).
         *
         * The single gate for every access to SpanImpl state. This event is
         * owned by the SpanData, which outlives the SpanImpl whenever a chunk
         * is still in flight — and user code may hold this event as a raw
         * SpanEventPtr just as long (see doc/api_contracts.md sections 4/5).
         * Dereferencing the span unguarded from such a late call would be a
         * use-after-free, so no new method may reach the span any other way.
         */
        SpanImpl* spanIfAlive() const;

        /// @brief True (after logging a warning) once the event is finished,
        /// signalling that a recording accessor or mutator must no-op. A
        /// finished event may already sit in a chunk under serialization on
        /// the gRPC worker, so mutating a field it reads (string reassignment,
        /// annotation-list growth) would be a data race.
        bool warnIfFinished() const;

        // Non-owning by design: this event lives inside that SpanData, so
        // the pointer is valid for as long as the event itself is, and hot
        // event operations pay no weak_ptr::lock() or refcount traffic. The
        // parent span is reached through it (see spanIfAlive) rather than
        // being cached here, because the span dies first.
        SpanData* data_;
        AgentService* agent_;
        int32_t service_type_;
        std::string operation_;
        int32_t sequence_{0};
        int32_t depth_{0};
        int64_t start_time_;
        // Caller-preset end timestamp (epoch ms) for batch-replayed events;
        // 0 means finish() stamps the wall clock as before.
        int64_t end_time_{0};
        int32_t start_elapsed_{0};
        int32_t elapsed_{0};
        int64_t next_span_id_{0};
        std::string endpoint_;
        std::string destination_id_;
        int32_t error_func_id_{0};
        std::string error_string_;
        // Id shared by every call stack recorded on this event (one exception
        // chain); 0 until the first one is buffered.
        int64_t exception_id_{0};
        int32_t async_id_{NONE_ASYNC_ID};
        int32_t async_seq_gen_{0};
        int32_t api_id_{0};
        // Idempotency guard for EndEvent, same shape as SpanImpl::finished_:
        // the atomic exchange lets only the first end proceed; NOT a
        // concurrency guarantee (events follow the span's single-thread
        // contract). Also set by finish(), so an event ended through an
        // internal path (e.g. async-span EndSpan) rejects a later EndEvent.
        std::atomic<bool> finished_{false};
        // Owned by value: an annotation-free event pays no heap.
        // releaseRetiredPayload() frees the list once the chunk is done with
        // it; the empty husk stays as the tombstone.
        PinpointAnnotation annotations_;
    };

    /**
     * @brief Span event handed out when the span's event stack has overflowed
     *        (max depth/sequence reached), mirroring the Java agent's
     *        DisableSpanEvent: nothing is recorded locally, but InjectContext
     *        still writes the full trace context so the distributed trace is
     *        not cut at the overflow point — overflow is a profiling depth
     *        limit, not a sampling decision.
     *
     * One instance per span, created lazily on first overflow and owned by the
     * span's SpanData — not by the SpanImpl — so a user-held pointer to it has
     * exactly the lifetime of a real span event's (see SpanData::getOwner).
     * Since every overflowed event of the span shares it, the kept destination
     * reflects the most recent SetDestination call (used only for the
     * Pinpoint-Host header).
     */
    class DisabledSpanEvent final : public SpanEvent {
    public:
        explicit DisabledSpanEvent(SpanData* data) : data_(data) {}
        ~DisabledSpanEvent() override {}

        void SetServiceType(int32_t type) override {}
        void SetOperationName(std::string_view operation) override {}
        void SetStartTime(std::chrono::system_clock::time_point start_time) override {}
        // Destination is kept (not recorded) so InjectContext can still write
        // the Pinpoint-Host header. Out-of-line: the assignment allocates, so
        // it needs the exception boundary in span_event.cpp.
        void SetDestination(std::string_view dest) override;
        void SetEndPoint(std::string_view end_point) override {}
        void SetError(std::string_view error_message) override {}
        void SetError(std::string_view error_name, std::string_view error_message) override {}
        void SetError(std::string_view error_name, std::string_view error_message, CallStackReader& reader) override {}
        void SetError(std::string_view error_name, std::string_view error_message,
                      const std::vector<CallStackFrame>& frames) override {}
        void SetSqlQuery(std::string_view sql_query,
                         const std::vector<SqlBindValue>& bind_args) override {}
        void RecordHeader(HeaderType which, HeaderReader& reader) override {}
        void InjectContext(TraceContextWriter& writer) override;
        // The overflowed event is never recorded, so a caller-generated child
        // span id is not stored either — same as InjectContext above.
        void SetNextSpanId(int64_t next_span_id) override {}

        void SetAnnotation(int32_t key, int32_t value) override {}
        void SetAnnotation(int32_t key, int64_t value) override {}
        void SetAnnotation(int32_t key, std::string_view value) override {}
        void SetAnnotation(int32_t key,
                           std::string_view value1,
                           std::string_view value2) override {}
        void EndEvent() override;

    private:
        // The owning SpanData; the parent span is reached through it only
        // while it is still alive (see InjectContext).
        SpanData* data_;
        std::string destination_id_;
    };

}  // namespace pinpoint
