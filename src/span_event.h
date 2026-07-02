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

#include "pinpoint/tracer.h"
#include "annotation.h"
#include "utility.h"

namespace pinpoint {

    class AgentService;
    class SpanImpl;

    /**
     * @brief Concrete span event implementation that records timing and metadata.
     */
    class SpanEventImpl final : public SpanEvent {
    public:
        SpanEventImpl(SpanImpl* span, std::string_view operation);
        ~SpanEventImpl() override {}

        /// @brief Sets the service type for this event.
        void SetServiceType(int32_t type) override { service_type_ = type; }
        /// @brief Sets the logical operation name.
        void SetOperationName(std::string_view operationName) override { operation_ = operationName; }
        /// @brief Records the absolute start time.
        void SetStartTime(std::chrono::system_clock::time_point start_time) override { start_time_ = to_milli_seconds(start_time); }
        /// @brief Records the destination identifier.
        void SetDestination(std::string_view dest) override {destination_id_ = dest; }
        /// @brief Records the remote endpoint.
        void SetEndPoint(std::string_view endpoint) override { endpoint_ = endpoint; }
        void SetError(std::string_view error_message) override;
        void SetError(std::string_view error_name, std::string_view error_message) override;
        void SetError(std::string_view error_name, std::string_view error_message, CallStackReader& reader) override;
        void SetSqlQuery(std::string_view sql_query, std::string_view args) override;
        void RecordHeader(HeaderType which, HeaderReader& reader) override;
        /// @brief Injects the trace context for the outbound call represented
        /// by this event (trace id, generated child span id, parent app info)
        /// into an outbound propagation carrier.
        void InjectContext(TraceContextWriter& writer) override;
        AnnotationPtr GetAnnotations() const override { return ensureAnnotations(); }
        void EndEvent() override;

        /**
         * @brief Finalizes the span event by computing elapsed metrics.
         */
        void finish();

        /// @brief Returns the service type identifier.
        int32_t getServiceType() const { return service_type_; }
        /// @brief Returns the recorded operation name.
        std::string& getOperationName() { return operation_; }

        /// @brief Returns the absolute start time in milliseconds.
        int64_t getStartTime() const { return start_time_; }
        /// @brief Sets the start offset relative to the parent span.
        void setStartElapsed(int32_t elapsed) { start_elapsed_ = elapsed; }
        /// @brief Returns the start offset relative to the parent span.
        int32_t getStartElapsed() const { return start_elapsed_; }
        /// @brief Returns the elapsed duration of the event.
        int32_t getEndElapsed() const { return elapsed_; }

        /// @brief Assigns the sequence number.
        void setSequence(int32_t sequence) { sequence_ = sequence; }
        /// @brief Returns the sequence number assigned to this event.
        int32_t getSequence() const { return sequence_; }
        /// @brief Sets the depth of this event in the call hierarchy.
        void setDepth(int32_t depth) { depth_ = depth; }
        /// @brief Returns the depth of this event in the call hierarchy.
        int32_t getDepth() const { return depth_; }

        /**
         * @brief Generates the next span identifier for asynchronous spans.
         */
        int64_t generateNextSpanId();
        /// @brief Returns the generated asynchronous span identifier.
        int64_t getNextSpanId() const { return next_span_id_; }

        /// @brief Returns the mutable annotation container, allocating it on
        /// first use if it has not been created yet.
        PinpointAnnotation* getAnnotations() { return ensureAnnotations(); }

        /// @brief Returns the recorded endpoint.
        std::string& getEndPoint() { return endpoint_; }
        /// @brief Returns the recorded destination identifier.
        std::string& getDestinationId() { return destination_id_; }

        /// @brief Returns the error function identifier, if any.
        int32_t getErrorFuncId() const { return error_func_id_; }
        /// @brief Returns the error message captured during execution.
        std::string& getErrorString() { return error_string_; }

        /// @brief Sets the asynchronous identifier for the event.
        void setAsyncId(const int32_t async_id) { async_id_ = async_id; }
        /// @brief Returns the asynchronous identifier.
        int32_t getAsyncId() const { return async_id_; }

        /// @brief Increments the async sequence generator.
        void incrAsyncSeq() { async_seq_gen_++; }
        /// @brief Returns the current async sequence value.
        int32_t getAsyncSeqGen() const { return async_seq_gen_; }

        /// @brief Assigns the API identifier associated with this event.
        void setApiId(int32_t api_id) { api_id_ = api_id; }
        /// @brief Returns the API identifier.
        int32_t getApiId() const { return api_id_; }

    private:
        /// @brief Lazily allocates the annotation container on first use and
        /// returns it. Subsequent calls reuse the same instance, so callers can
        /// rely on a non-null result. Kept const (with a mutable backing field)
        /// so the const GetAnnotations() override can materialize it on demand.
        PinpointAnnotation* ensureAnnotations() const;

        // Non-owning by design. SpanData owns SpanEventImpl instances and keeps
        // them within the parent span's lifetime, while this pointer lets hot
        // event operations avoid weak_ptr::lock() and shared_ptr refcount traffic.
        SpanImpl* span_;
        AgentService* agent_;
        int32_t service_type_;
        std::string operation_;
        int32_t sequence_;
        int32_t depth_;
        int64_t start_time_;
        int32_t start_elapsed_;
        int32_t elapsed_;
        int64_t next_span_id_;
        std::string endpoint_;
        std::string destination_id_;
        int32_t error_func_id_;
        std::string error_string_;
        int32_t async_id_;
        int32_t async_seq_gen_;
        int32_t api_id_;
        // Created lazily via ensureAnnotations(); stays null until the first
        // annotation is recorded or the container is accessed.
        mutable std::unique_ptr<PinpointAnnotation> annotations_;
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
     * SpanImpl that hands it out, so the pointer stays valid for the span's
     * whole lifetime. Because the instance is shared by every overflowed event
     * of the span, the kept destination reflects the most recent SetDestination
     * call (only used for the Pinpoint-Host header).
     */
    class DisabledSpanEvent final : public SpanEvent {
    public:
        explicit DisabledSpanEvent(SpanImpl* span) : span_(span) {}
        ~DisabledSpanEvent() override {}

        void SetServiceType(int32_t type) override {}
        void SetOperationName(std::string_view operation) override {}
        void SetStartTime(std::chrono::system_clock::time_point start_time) override {}
        /// @brief Destination is kept (not recorded) so InjectContext can still
        /// write the Pinpoint-Host header for the outbound call.
        void SetDestination(std::string_view dest) override { destination_id_ = dest; }
        void SetEndPoint(std::string_view end_point) override {}
        void SetError(std::string_view error_message) override {}
        void SetError(std::string_view error_name, std::string_view error_message) override {}
        void SetError(std::string_view error_name, std::string_view error_message, CallStackReader& reader) override {}
        void SetSqlQuery(std::string_view sql_query, std::string_view args) override {}
        void RecordHeader(HeaderType which, HeaderReader& reader) override {}
        void InjectContext(TraceContextWriter& writer) override;

        AnnotationPtr GetAnnotations() const override;
        void EndEvent() override;

    private:
        SpanImpl* span_;
        std::string destination_id_;
    };

}  // namespace pinpoint
