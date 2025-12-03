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

#include <mutex>
#include <stack>
#include <vector>

#include "agent_service.h"
#include "callstack.h"
#include "config.h"
#include "span_event.h"
#include "url_stat.h"
#include "utility.h"

namespace pinpoint {

    /**
     * @brief Thread-safe stack wrapper used to manage nested span events.
     */
    class EventStack {
    public:
        EventStack() = default;

        /**
         * @brief Pushes a span event onto the internal stack.
         *
         * @param item Span event to add.
         */
        void push(const std::shared_ptr<SpanEventImpl>& item) {
            std::unique_lock<std::mutex> lock(mutex_);
            stack_.push(item);
        }

        /**
         * @brief Removes and returns the most recent span event.
         *
         * @return Span event that was at the top of the stack.
         */
        std::shared_ptr<SpanEventImpl> pop() {
            std::unique_lock<std::mutex> lock(mutex_);
            auto item = stack_.top();
            stack_.pop();
            return item;
        }

        /**
         * @brief Returns (without removing) the top span event.
         */
        std::shared_ptr<SpanEventImpl> top() {
            std::unique_lock<std::mutex> lock(mutex_);
            return stack_.top();
        }

        /// @brief Returns the number of events contained in the stack.
        size_t size() const { return stack_.size(); }

    private:
        std::mutex mutex_;
        std::stack<std::shared_ptr<SpanEventImpl>> stack_;
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
        SpanData(AgentService* agent, std::string_view operation);
        ~SpanData() = default;

    	/// @brief Returns the trace identifier.
    	TraceId& getTraceId() { return trace_id_; }
    	/// @brief Sets the trace identifier.
    	void setTraceId(const TraceId& trace_id) { trace_id_ = trace_id; }
    	/**
    	 * @brief Parses a textual trace ID into the internal representation.
    	 *
    	 * @param tid String containing the trace identifier.
    	 */
    	void parseTraceId(std::string &tid) noexcept;

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
        void setLoggingFlag() { logging_flag_ = 1; }
        /// @brief Returns logging verbosity information.
        int32_t getLoggingFlag() const { return logging_flag_; }

        /// @brief Sets span flag bits.
        void setFlags(int flags) { flags_ = flags; }
        /// @brief Returns span flag bits.
        int getFlags() const { return flags_; }

        /// @brief Sets the event sequence counter.
        void setEventSequence(int32_t event_sequence) { event_sequence_ = event_sequence; }
        /// @brief Returns the event sequence counter.
        int32_t getEventSequence() const { return event_sequence_; }

        /// @brief Sets the current event depth.
        void setEventDepth(int32_t event_depth) { event_depth_ = event_depth; }
        /// @brief Returns the current event depth.
        int32_t getEventDepth() const { return event_depth_; }

        /// @brief Decrements the event depth counter.
        void decrEventDepth() { event_depth_--; }

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

    	/**
    	 * @brief Captures URL statistics for the span using the given metadata.
    	 *
    	 * @param url_pattern Normalized URL template.
    	 * @param method HTTP method.
    	 * @param status_code HTTP status code.
    	 */
    	void setUrlStat(std::string_view url_pattern, std::string_view method, int status_code);
    	/**
    	 * @brief Enqueues the recorded URL statistics for asynchronous sending.
    	 */
    	void sendUrlStat();
		std::string getUrlTemplate() {
			if (url_stat_) {
				return url_stat_->url_pattern_;
			}
			return "NULL";
		}

    	/// @brief Stores the start timestamp of the span in epoch milliseconds.
    	void setStartTime(std::chrono::system_clock::time_point start_time) { start_time_ = to_milli_seconds(start_time); }
        /// @brief Returns the recorded start timestamp.
        int64_t getStartTime() const { return start_time_; }

    	/// @brief Captures the end time and computes the elapsed duration.
    	void setEndTime() {
	        end_time_ = std::chrono::system_clock::now();
        	elapsed_ = to_milli_seconds(end_time_) - start_time_;
        }
        /// @brief Returns the elapsed duration in milliseconds.
        int32_t getElapsed() const { return elapsed_; }

    	/**
    	 * @brief Pushes a newly created span event onto the event stack.
    	 *
    	 * @param se Span event to track.
    	 */
    	void addSpanEvent(const std::shared_ptr<SpanEventImpl>& se);
    	/**
    	 * @brief Finalizes the top span event and moves it into the finished list.
    	 */
    	void finishSpanEvent();
    	/// @brief Returns the current active span event.
    	std::shared_ptr<SpanEventImpl> topSpanEvent() { return event_stack_.top(); }

        /// @brief Returns the collection of finished span events.
        std::vector<std::shared_ptr<SpanEventImpl>>& getFinishedEvents() { return finished_events; }
    	/// @brief Returns the number of finished span events.
    	size_t getFinishedEventsCount() const { return finished_events.size(); }
    	/// @brief Clears the list of finished span events.
    	void clearFinishedEvents() { finished_events.clear(); }

        /// @brief Appends a captured exception call stack.
        void addException(std::unique_ptr<Exception> exception) { exceptions_.push_back(std::move(exception)); }
        /// @brief Transfers the collected exceptions to the caller.
        std::vector<std::unique_ptr<Exception>> getExceptions() { return std::move(exceptions_); }
    	/**
    	 * @brief Streams the collected exceptions through the agent service.
    	 */
    	void sendExceptions();

        /// @brief Returns the annotation container for the span.
        std::shared_ptr<PinpointAnnotation> getAnnotations() const { return annotations_; }
    	/// @brief Returns the owning agent service.
    	AgentService* getAgent() const { return agent_; }

    private:
    	TraceId trace_id_;
    	int64_t span_id_;

    	int64_t parent_span_id_;
    	std::string parent_app_name_;
    	int32_t parent_app_type_;
    	std::string parent_app_namespace_;

    	int32_t app_type_;
    	int32_t service_type_;
    	std::string operation_;
    	int32_t api_id_;

    	std::string rpc_name_;
    	std::string endpoint_;
    	std::string remote_addr_;
    	std::string acceptor_host_;

    	int32_t event_sequence_;
    	int32_t event_depth_;

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
        std::vector<std::shared_ptr<SpanEventImpl>> finished_events;
    	std::mutex span_event_lock_;

        std::unique_ptr<UrlStat> url_stat_;
    	std::shared_ptr<PinpointAnnotation> annotations_;
        std::vector<std::unique_ptr<Exception>> exceptions_;
    	AgentService *agent_;
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
		std::vector<std::shared_ptr<SpanEventImpl>>& getSpanEventChunk() { return event_chunk_; }
		/// @brief Timestamp used for ordering span chunks.
		int64_t getKeyTime() const { return key_time_; }
		/// @brief Indicates whether this chunk represents the final events of the span.
		bool isFinal() const { return final_; }

	private:
		std::shared_ptr<SpanData> span_data_;
		std::vector<std::shared_ptr<SpanEventImpl>> event_chunk_;
		bool final_;
		int64_t key_time_;
	};

    /**
     * @brief Concrete span implementation used when tracing is enabled.
     *
     * `SpanImpl` delegates storage to `SpanData` while coordinating span event creation,
     * context propagation and final flushing through the agent service.
     */
    class SpanImpl final : public Span {
    public:
        SpanImpl(AgentService* agent, std::string_view operation, std::string_view rpc_point);
        ~SpanImpl() override = default;

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
      	/// @brief Finalizes the current span event.
      	void EndSpanEvent() override;
      	/// @brief Finalizes the span and schedules it for flushing.
      	void EndSpan() override;
    	/**
    	 * @brief Creates a child span used for asynchronous work.
    	 *
    	 * @param async_operation Logical operation name for the async span.
    	 */
    	SpanPtr NewAsyncSpan(std::string_view async_operation) override;

      	/// @brief Injects the current span context into an outbound propagation carrier.
      	void InjectContext(TraceContextWriter& writer) override;
      	/// @brief Extracts a span context from an inbound propagation carrier.
      	void ExtractContext(TraceContextReader& reader) override;

    	TraceId& GetTraceId() override { return data_->getTraceId(); }
    	int64_t GetSpanId() override { return data_->getSpanId(); }
    	bool IsSampled() override { return true; }
    	AnnotationPtr GetAnnotations() const override { return data_->getAnnotations(); }

    	/// @brief Sets the service type recorded on the span.
    	void SetServiceType(int32_t service_type) override;
    	/// @brief Records the start timestamp of the span.
    	void SetStartTime(std::chrono::system_clock::time_point start_time) override;
        /// @brief Records the remote network address.
        void SetRemoteAddress(std::string_view address) override;
        /// @brief Records the service endpoint.
        void SetEndPoint(std::string_view end_point) override;
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
		AgentService *agent_;
    	std::shared_ptr<SpanData> data_;
    	int32_t overflow_;
    	bool finished_;

    	/**
    	 * @brief Emits a span chunk via the agent service.
    	 *
    	 * @param final Indicates whether the chunk completes the span.
    	 */
    	void record_chunk(bool final) const;
    };

}  // namespace pinpoint

