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
#include <deque>
#include <memory>
#include <optional>
#include <stack>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agent_service.h"
#include "callstack.h"
#include "config.h"
#include "span_event.h"
#include "url_stat.h"
#include "utility.h"

namespace pinpoint {
    
    // Span status and flag constants
    constexpr int SPAN_FLAG_NONE = 0;
    constexpr int SPAN_ERR_NONE = 0;
    constexpr int SPAN_ERR_TRUE = 1;
    constexpr int32_t SPAN_LOGGING_FLAG_OFF = 0;
    constexpr int32_t SPAN_LOGGING_FLAG_ON = 1;

    /**
     * @brief Stack wrapper used to manage nested span events.
     *
     * This class is NOT thread-safe on its own, and does not need to be: a span
     * is owned by a single thread (see the Span thread-safety contract in
     * pinpoint/tracer.h), so all access happens on that one thread.
     */
    class EventStack {
    public:
        EventStack() = default;

        /**
         * @brief Pushes a span event onto the internal stack.
         *
         * @param item Span event to add.
         * @note Must be called on the span's owning thread.
         */
        void push(std::unique_ptr<SpanEventImpl> item) {
            stack_.push(std::move(item));
        }

        /**
         * @brief Removes and returns the most recent span event.
         *
         * @return Span event that was at the top of the stack, or nullptr if the stack is empty.
         * @note Must be called on the span's owning thread.
         */
        std::unique_ptr<SpanEventImpl> pop() {
            if (stack_.empty()) {
                return nullptr;
            }
            auto item = std::move(stack_.top());
            stack_.pop();
            return item;
        }

        /**
         * @brief Returns (without removing) the top span event.
         * @return Span event at the top of the stack, or nullptr if the stack is empty.
         * @note Must be called on the span's owning thread.
         */
        SpanEventImpl* top() {
            if (stack_.empty()) {
                return nullptr;
            }
            return stack_.top().get();
        }

        /// @brief Returns the number of events contained in the stack.
        /// @note Must be called on the span's owning thread.
        size_t size() const {
            return stack_.size();
        }

    private:
        std::stack<std::unique_ptr<SpanEventImpl>> stack_;
    };

    /**
     * @brief Holds mutable state for a span until it is serialized and flushed.
     *
     * `SpanData` collects identifiers, network attributes, annotations, child events and
     * exceptions. When the span ends the data is converted into one or multiple `SpanChunk`
     * messages destined for the collector.
     */
    class SpanData final {
    public:
        SpanData(std::string_view operation, int32_t app_type, int32_t api_id);
        ~SpanData() = default;

    	/// @brief Returns the trace identifier.
    	TraceId& getTraceId() { return trace_id_; }
    	/// @brief Sets the trace identifier.
    	// By value so an rvalue (setTraceId(generateTraceId())) moves straight
    	// through without copying the AgentId string a second time.
    	void setTraceId(TraceId trace_id) { trace_id_ = std::move(trace_id); }

    	/// @brief Stores the numeric span identifier.
    	void setSpanId(int64_t span_id) { span_id_ = span_id; }
    	/// @brief Returns the numeric span identifier.
    	int64_t getSpanId() const { return span_id_; }

    	/// @brief Returns the application type.
    	int32_t getAppType() const { return app_type_; }
        /// @brief Returns the logical operation name.
        std::string& getOperationName() { return operation_; }
    	/// @brief Returns the cached API identifier for the operation.
    	int32_t getApiId() const { return api_id_; }

        /// @brief Sets the identifier of the parent span.
        void setParentSpanId(int64_t parent_span_id) { parent_span_id_ = parent_span_id; }
        /// @brief Returns the identifier of the parent span.
        int64_t getParentSpanId() const { return parent_span_id_; }

        /// @brief Sets the application type of the parent span.
        void setParentAppType(int parent_app_type) { parent_app_type_ = parent_app_type; }
        /// @brief Returns the application type of the parent span.
        int32_t getParentAppType() const { return parent_app_type_; }

        /// @brief Sets the parent application name.
        void setParentAppName(std::string_view parent_app_name) { parent_app_name_ = parent_app_name; }
        /// @brief Returns the parent application name.
        std::string& getParentAppName() { return parent_app_name_; }

        /// @brief Sets the parent application namespace.
        void setParentAppNamespace(std::string_view parent_app_namespace) { parent_app_namespace_ = parent_app_namespace; }
        /// @brief Returns the parent application namespace.
        std::string& getParentAppNamespace() { return parent_app_namespace_; }

        /// @brief Sets the parent service name.
        void setParentServiceName(std::string_view parent_service_name) { parent_service_name_ = parent_service_name; }
        /// @brief Returns the parent service name.
        std::string& getParentServiceName() { return parent_service_name_; }

        /// @brief Sets the service type associated with this span.
        void setServiceType(int service_type) { service_type_ = service_type; }
        /// @brief Returns the service type associated with this span.
        int32_t getServiceType() const { return service_type_; }

        /// @brief Sets the RPC name for the span.
        void setRpcName(std::string_view rpc_name) { rpc_name_ = rpc_name; }
        /// @brief Returns the RPC name for the span.
        std::string& getRpcName() { return rpc_name_; }

        /// @brief Sets the endpoint that handled the request.
        void setEndPoint(std::string_view endpoint) { endpoint_ = endpoint; }
        /// @brief Returns the endpoint that handled the request.
        std::string& getEndPoint() { return endpoint_; }

        /// @brief Sets the remote address of the client.
        void setRemoteAddr(std::string_view remote_addr) { remote_addr_ = remote_addr; }
        /// @brief Returns the remote address of the client.
        std::string& getRemoteAddr() { return remote_addr_; }

        /// @brief Sets the acceptor host recorded for this span.
        void setAcceptorHost(std::string_view acceptor_host) { acceptor_host_ = acceptor_host; }
        /// @brief Returns the acceptor host recorded for this span.
        std::string& getAcceptorHost() { return acceptor_host_; }

        /// @brief Sets logging verbosity information.
        void setLoggingFlag() { logging_flag_ = SPAN_LOGGING_FLAG_ON; }
        /// @brief Returns logging verbosity information.
        int32_t getLoggingFlag() const { return logging_flag_; }

        /// @brief Sets span flag bits.
        void setFlags(int flags) { flags_ = flags; }
        /// @brief Returns span flag bits.
        int getFlags() const { return flags_; }

        /// @brief Returns the event sequence counter.
        int32_t getEventSequence() const { return event_sequence_.load(std::memory_order_relaxed); }
        /// @brief Returns the current event depth.
        int32_t getEventDepth() const { return event_depth_.load(std::memory_order_relaxed); }
        /// @brief Reserves the next event sequence and depth for a new span event.
        std::pair<int32_t, int32_t> nextEventSequenceAndDepth() {
            return {
                event_sequence_.fetch_add(1, std::memory_order_relaxed),
                event_depth_.fetch_add(1, std::memory_order_relaxed)
            };
        }
        /// @brief Decrements the event depth counter.
        void decrEventDepth() { event_depth_.fetch_sub(1, std::memory_order_relaxed); }

        /// @brief Sets the error code associated with the span.
        void setErr(int err) { err_ = err; }
        /// @brief Returns the error code associated with the span.
        int getErr() const { return err_; }

        /// @brief Sets the error function identifier.
        void setErrorFuncId(int32_t error_func_id) { error_func_id_ = error_func_id; }
    	/// @brief Returns the error function identifier.
    	int32_t getErrorFuncId() const { return error_func_id_; }

        /// @brief Sets the error message for the span.
        void setErrorString(std::string_view error_string) { error_string_ = error_string; }
    	/// @brief Returns the error message for the span.
    	const std::string& getErrorString() const { return error_string_; }

    	/// @brief Sets the asynchronous identifier.
    	void setAsyncId(int32_t async_id) { async_id_ = async_id; }
        /// @brief Returns the asynchronous identifier.
        int32_t getAsyncId() const { return async_id_; }
    	/// @brief Returns whether the span represents asynchronous work.
    	bool isAsyncSpan() const { return async_id_ != NONE_ASYNC_ID; }

    	/// @brief Sets the asynchronous sequence number.
    	void setAsyncSequence(int32_t async_seq) { async_sequence_ = async_seq; }
        /// @brief Returns the asynchronous sequence number.
        int32_t getAsyncSequence() const { return async_sequence_; }

    	/// @brief Stores the start timestamp of the span in epoch milliseconds.
    	void setStartTime(std::chrono::system_clock::time_point start_time) { start_time_ = to_milli_seconds(start_time); }
        /// @brief Returns the recorded start timestamp.
        int64_t getStartTime() const { return start_time_; }

    	/// @brief Captures the end time and computes the elapsed duration.
    	void setEndTime() {
	        end_time_ = std::chrono::system_clock::now();
    	    // system_clock can step backwards (NTP); never report a negative
    	    // elapsed time.
        	elapsed_ = static_cast<int32_t>(std::max<int64_t>(to_milli_seconds(end_time_) - start_time_, 0));
        }
        std::chrono::system_clock::time_point getEndTime() const { return end_time_; }
        /// @brief Returns the elapsed duration in milliseconds.
        int32_t getElapsed() const { return elapsed_; }

    	/**
    	 * @brief Pushes a newly created span event onto the event stack.
    	 *
    	 * @param se Span event to track.
    	 */
    	SpanEventImpl* addSpanEvent(std::unique_ptr<SpanEventImpl> se);
    	/**
    	 * @brief Finalizes the top span event and moves it into the finished list.
    	 */
    	void finishSpanEvent();
    	/**
    	 * @brief Finalizes every span event still on the stack (LIFO order).
    	 *
    	 * Called at EndSpan so events user code failed to end — plus the async
    	 * root event, which stays open until EndSpan by design — are recorded
    	 * in the final chunk instead of being silently dropped.
    	 *
    	 * @return Number of events that were still open.
    	 */
    	size_t finishOpenSpanEvents();
    	/// @brief Returns the current active span event, or nullptr if the stack is empty.
    	SpanEventImpl* topSpanEvent() {
    	    return event_stack_.top();
    	}

        /**
         * @brief Drains finished span events into a chunk view.
         *
         * Ownership stays with this SpanData: the events move to the retired
         * list and `out` receives borrowed pointers that remain valid for the
         * lifetime of this SpanData. Retaining ownership here is what keeps a
         * user-held raw SpanEventPtr safe after a mid-span chunk flush — the
         * duplicate-EndEvent no-op documented in pinpoint/tracer.h must never
         * touch freed memory.
         */
        void takeFinishedEvents(std::vector<SpanEventImpl*>& out);
        /// @brief Drains finished span events, returning borrowed pointers.
        std::vector<SpanEventImpl*> takeFinishedEvents() {
            std::vector<SpanEventImpl*> events;
            takeFinishedEvents(events);
            return events;
        }
        /// @brief Returns the number of finished span events.
    	size_t getFinishedEventsCount() const {
    	    return finished_events.size();
    	}

        /// @brief Returns the annotation container for the span.
        PinpointAnnotation* getAnnotations() const { return annotations_.get(); }

    private:
        void storeFinishedEvent(std::unique_ptr<SpanEventImpl> se);

    	TraceId trace_id_;
    	int64_t span_id_;

    	int64_t parent_span_id_;
    	std::string parent_app_name_;
    	int32_t parent_app_type_;
    	std::string parent_app_namespace_;
    	std::string parent_service_name_;

    	int32_t app_type_;
    	int32_t service_type_;
    	std::string operation_;
    	int32_t api_id_;

    	std::string rpc_name_;
    	std::string endpoint_;
    	std::string remote_addr_;
    	std::string acceptor_host_;

        // Atomic so overflow checks and event position reservation never race
        // concurrent NewSpanEvent calls.
    	std::atomic<int32_t> event_sequence_;
    	std::atomic<int32_t> event_depth_;

    	int32_t logging_flag_;
    	int flags_;
    	int err_;
    	int32_t error_func_id_;
    	std::string error_string_;

    	int64_t start_time_;
    	std::chrono::system_clock::time_point end_time_;
    	int32_t elapsed_;

    	int32_t async_id_;
    	int32_t async_sequence_;

    	EventStack event_stack_;
        // Kept sequence-ordered as events finish so chunks do not need to sort.
        // Not mutex-guarded: a span is single-threaded (see the Span thread-safety
        // contract in pinpoint/tracer.h), so the stack and this list are only ever
        // touched by the span's owning thread.
        std::deque<std::unique_ptr<SpanEventImpl>> finished_events;
        // Finished events already handed to a chunk. Ownership is retained
        // here (a chunk holds borrowed pointers plus a shared_ptr to this
        // SpanData), so raw SpanEventPtr handles held by user code stay valid
        // until the span data itself is released: a late duplicate EndEvent
        // after a chunk flush lands on a live object and stays the safe no-op
        // the public headers promise, instead of a use-after-free.
        std::vector<std::unique_ptr<SpanEventImpl>> retired_events_;

    	std::unique_ptr<PinpointAnnotation> annotations_;
    };

	/**
	 * @brief Represents a batch of span events emitted as a single gRPC message.
	 */
	class SpanChunk final {
	public:
		SpanChunk(const std::shared_ptr<SpanData>& span_data, bool final);
		~SpanChunk() = default;

		/**
		 * @brief Compacts the span event list by removing completed events.
		 */
		void optimizeSpanEvents();

		/// @brief Returns the parent span data associated with this chunk.
		std::shared_ptr<SpanData>& getSpanData() { return span_data_; }
		/// @brief Returns the span events contained in this chunk.
		std::vector<SpanEventImpl*>& getSpanEventChunk() { return event_chunk_; }
		/// @brief Timestamp used for ordering span chunks.
		int64_t getKeyTime() const { return key_time_; }
		/// @brief Indicates whether this chunk represents the final events of the span.
		bool isFinal() const { return final_; }

	private:
		std::shared_ptr<SpanData> span_data_;
		// Borrowed from span_data_'s retired list; kept alive by span_data_.
		std::vector<SpanEventImpl*> event_chunk_;
		bool final_;
		int64_t key_time_;
	};

    /**
     * @brief Concrete span implementation used when tracing is enabled.
     *
     * `SpanImpl` delegates storage to `SpanData` while coordinating span event creation,
     * context propagation and final flushing through the agent service.
     *
     * @warning Single-threaded per instance. See the `Span` thread-safety contract
     *          in pinpoint/tracer.h: one `SpanImpl` (and the `SpanEventImpl`s it
     *          returns as raw `SpanEventPtr`) must be used by a single thread for
     *          its whole lifetime. Concurrent calls on the same instance are
     *          undefined behaviour and can crash — `exceptions_`, `url_stat_` and
     *          the `SpanData` string/annotation buffers are unsynchronized, and
     *          releasing the span on one thread frees the span events (retained
     *          in `SpanData`) still referenced through raw pointers on another.
     *
     *          The `finished_`/`overflow_` atomics are defensive idempotency
     *          guards (e.g. so a repeated EndSpan is a no-op), NOT a concurrency
     *          guarantee. There is no lock protecting the span event stack either:
     *          each span — including an async child from NewAsyncSpan(), which
     *          owns its own SpanData — is only ever touched by its single owning
     *          thread. Do not read any of this as license to share a span across
     *          threads.
     */
    class SpanImpl final : public Span, public std::enable_shared_from_this<SpanImpl> {
    public:
        // `config` is the creator's already-loaded config snapshot (e.g. the
        // AgentRuntime generation NewSpan sampled/filtered against). Passing it
        // skips the extra atomic runtime load agent->getConfig() would pay and
        // keeps the span on the exact config generation of its admission
        // decision. When omitted (tests, direct construction), the ctor loads
        // the current config itself.
        SpanImpl(AgentService* agent, std::string_view operation, std::string_view rpc_point,
                 std::shared_ptr<const Config> config = nullptr);
        ~SpanImpl() override;

    	SpanEventPtr NewSpanEvent(std::string_view operation) override {
    		return NewSpanEvent(operation, defaults::SPAN_EVENT_SERVICE_TYPE);
    	}
    	/**
    	 * @brief Creates a new span event associated with this span.
    	 *
    	 * @param operation Logical operation name.
    	 * @param service_type Service type identifier.
    	 * @return Newly created span event.
    	 */
    	SpanEventPtr NewSpanEvent(std::string_view operation, int32_t service_type) override;
        /// @brief Returns the currently active span event.
        SpanEventPtr GetSpanEvent() override;
      	/// @brief Finalizes the span and schedules it for flushing.
      	void EndSpan() override;
    	/**
    	 * @brief Creates a child span used for asynchronous work.
    	 *
    	 * @param async_operation Logical operation name for the async span.
    	 */
    	SpanPtr NewAsyncSpan(std::string_view async_operation) override;

    	/**
    	 * @brief Extracts a span context from an inbound propagation carrier.
    	 *
    	 * Impl-level only (no public Span counterpart): called by
    	 * AgentImpl::NewSpan right after span creation.
    	 *
    	 * @param reader Trace context reader provided by user code.
    	 * @param trace_id The already-resolved trace id for this span — parsed
    	 *                 from the inbound HEADER_TRACE_ID or freshly generated by
    	 *                 NewSpan before this call. Must be non-empty (NewSpan
    	 *                 turns an empty/failed trace id into a noop span instead
    	 *                 of reaching here). Moved into the span's SpanData.
    	 */
    	void extractContext(TraceContextReader& reader, TraceId trace_id);
    	/**
    	 * @brief Injects this span's context into an outbound propagation carrier.
    	 *
    	 * Impl-level only: called by the span events this span hands out
    	 * (SpanEventImpl and DisabledSpanEvent) to implement
    	 * SpanEvent::InjectContext. No-op once the span is finished.
    	 *
    	 * @param writer Outbound carrier writer.
    	 * @param next_span_id Child span id generated for the outbound call.
    	 * @param host Destination of the outbound call (Pinpoint-Host header).
    	 */
    	void injectContext(TraceContextWriter& writer, int64_t next_span_id, std::string_view host);
    	/**
    	 * @brief Pops and finalizes the top span event of this span's stack.
    	 *
    	 * Impl-level only (no public Span counterpart): called by
    	 * SpanEventImpl::EndEvent — user code ends an event through the event
    	 * handle, which guards against duplicate ends before delegating here.
    	 */
    	void endSpanEvent();
    	/**
    	 * @brief Consumes one pending overflow placeholder.
    	 *
    	 * Impl-level only: called by DisabledSpanEvent::EndEvent. Overflowed
    	 * events are never pushed onto the stack, so ending one must not pop a
    	 * real event; it only balances the overflow counter kept by
    	 * NewSpanEvent. Warns when there is no pending overflow (duplicate end).
    	 */
    	void endDisabledSpanEvent();

        std::string GetTraceId() override { return data_->getTraceId().toString(); }
        int64_t GetSpanId() override { return data_->getSpanId(); }
        bool IsSampled() override { return true; }
        AnnotationPtr GetAnnotations() const override { return data_->getAnnotations(); }
        const std::shared_ptr<SpanData>& getSpanData() const { return data_; }
        const std::vector<std::unique_ptr<Exception>>& getExceptions() const { return exceptions_; }
        std::vector<std::unique_ptr<Exception>> takeExceptions() { return std::move(exceptions_); }
        std::string getUrlTemplate() const {
            if (url_stat_) {
                return url_stat_->url_pattern_;
            }
            return "NULL";
        }

    	/// @brief Sets the service type recorded on the span.
    	void SetServiceType(int32_t service_type) override;
    	/// @brief Records the start timestamp of the span.
    	void SetStartTime(std::chrono::system_clock::time_point start_time) override;
        /// @brief Records the remote network address.
        void SetRemoteAddress(std::string_view address) override;
        /// @brief Records the service endpoint.
        void SetEndPoint(std::string_view end_point) override;
        /// @brief Records the host of acceptor.
        void SetAcceptorHost(std::string_view host) override;
    	/// @brief Records an error message and marks the span as failed.
    	void SetError(std::string_view error_message) override;
    	/// @brief Records a named error along with an error message.
    	void SetError(std::string_view error_name, std::string_view error_message) override;
    	/// @brief Records the HTTP status code returned by the operation.
    	void SetStatusCode(int status) override;
        /// @brief Captures URL statistics for the span.
        void SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) override;
        /// @brief Records the logging flag and injects the span context into a logger.
        void SetLogging(TraceContextWriter& writer) override;
    	/// @brief Records HTTP headers into span annotations.
    	void RecordHeader(HeaderType which, HeaderReader& reader) override;

	private:
	    friend class SpanEventImpl;

            // Keeps the agent alive while user code still holds this span,
            // the same protection UnsampledSpan::agent_ref_ provides: a span
            // may legally outlive Shutdown()/agent-handle destruction, and
            // EndSpan and the SpanEventImpl recorders dereference agent_.
            // Null only for agents that are not shared_ptr-owned
            // (stack-constructed test instances).
            std::shared_ptr<AgentService> agent_ref_;
            AgentService *agent_;
            // Config snapshot taken once at span creation. Per-event hot paths
            // (NewSpanEvent/EndEvent and the SpanEventImpl recorders) read
            // this instead of agent_->getConfig(), so they pay no atomic
            // shared_ptr load per call, and the span's limits (max_event_depth,
            // event_chunk_size, ...) stay consistent for its whole lifetime
            // even when a config reload lands mid-span.
            std::shared_ptr<const Config> config_;
            std::shared_ptr<SpanData> data_;
            std::atomic<int32_t> overflow_;
            std::atomic<bool> finished_;
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
            // call rather than at construction, so an async child span created
            // via NewAsyncSpan on one thread but consumed on another binds to its
            // consuming thread instead of its creator. The check is just a
            // relaxed thread-id load-and-compare (an already-bound span does
            // no atomic RMW, only the one-time binding uses a CAS),
            // cheap enough to stay enabled in release builds: a violation is
            // logged there, and additionally asserts in debug builds. Atomic so
            // the detection path itself — which by definition runs when the span
            // is touched concurrently — is not a data race.
            std::atomic<std::thread::id> owner_thread_id_{};
            void checkOwnerThread();

		/**
		 * @brief Emits a span chunk via the agent service.
		 *
		 * @param final Indicates whether the chunk completes the span.
		 */
            void record_chunk(bool final) const;
            void sendUrlStat();
            void sendExceptions();
            // Exceptions are only drained at EndSpan (unlike span events,
            // which chunk-flush mid-span), so a retry loop on a long-lived
            // span would otherwise grow this without bound — each entry
            // carries a full string callstack. Excess exceptions are dropped.
            static constexpr size_t kMaxBufferedExceptions = 100;
            // Returns false when the buffer is full and the exception is
            // dropped, so the caller can skip the ANNOTATION_EXCEPTION_ID
            // annotation that would otherwise reference a never-sent id.
            bool addException(std::unique_ptr<Exception> exception) {
                if (exceptions_.size() >= kMaxBufferedExceptions) {
                    return false;
                }
                exceptions_.push_back(std::move(exception));
                return true;
            }
            AgentService* getAgent() const { return agent_; }
            void decrEventDepth();
	};

}  // namespace pinpoint
