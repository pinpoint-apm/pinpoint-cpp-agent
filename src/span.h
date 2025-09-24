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
#include "span_event.h"
#include "url_stat.h"
#include "utility.h"

namespace pinpoint {

    class EventStack {
    public:
        EventStack() = default;

        void push(const std::shared_ptr<SpanEventImpl>& item) {
            std::unique_lock<std::mutex> lock(mutex_);
            stack_.push(item);
        }

        std::shared_ptr<SpanEventImpl> pop() {
            std::unique_lock<std::mutex> lock(mutex_);
            auto item = stack_.top();
            stack_.pop();
            return item;
        }

        std::shared_ptr<SpanEventImpl> top() {
            std::unique_lock<std::mutex> lock(mutex_);
            return stack_.top();
        }

        size_t size() const { return stack_.size(); }

    private:
        std::mutex mutex_;
        std::stack<std::shared_ptr<SpanEventImpl>> stack_;
    };

    class SpanData final {
    public:
        SpanData(AgentService* agent, std::string_view operation);
        ~SpanData() = default;

    	TraceId& getTraceId() { return trace_id_; }
    	void setTraceId(const TraceId& trace_id) { trace_id_ = trace_id; }
    	void parseTraceId(std::string &tid) noexcept;

    	void setSpanId(int64_t span_id) { span_id_ = span_id; }
    	int64_t getSpanId() const { return span_id_; }

    	int32_t getAppType() const { return app_type_; }
        std::string& getOperationName() { return operation_; }
    	int32_t getApiId() const { return api_id_; }

        void setParentSpanId(int64_t parent_span_id) { parent_span_id_ = parent_span_id; }
        int64_t getParentSpanId() const { return parent_span_id_; }

        void setParentAppType(int parent_app_type) { parent_app_type_ = parent_app_type; }
        int32_t getParentAppType() const { return parent_app_type_; }

        void setParentAppName(std::string_view parent_app_name) { parent_app_name_ = parent_app_name; }
        std::string& getParentAppName() { return parent_app_name_; }

        void setParentAppNamespace(std::string_view parent_app_namespace) { parent_app_namespace_ = parent_app_namespace; }
        std::string& getParentAppNamespace() { return parent_app_namespace_; }

        void setServiceType(int service_type) { service_type_ = service_type; }
        int32_t getServiceType() const { return service_type_; }

        void setRpcName(std::string_view rpc_name) { rpc_name_ = rpc_name; }
        std::string& getRpcName() { return rpc_name_; }

        void setEndPoint(std::string_view endpoint) { endpoint_ = endpoint; }
        std::string& getEndPoint() { return endpoint_; }

        void setRemoteAddr(std::string_view remote_addr) { remote_addr_ = remote_addr; }
        std::string& getRemoteAddr() { return remote_addr_; }

        void setAcceptorHost(std::string_view acceptor_host) { acceptor_host_ = acceptor_host; }
        std::string& getAcceptorHost() { return acceptor_host_; }

        void setLoggingInfo(int32_t logging_info) { logging_info_ = logging_info; }
        int32_t getLoggingInfo() const { return logging_info_; }

        void setFlags(int flags) { flags_ = flags; }
        int getFlags() const { return flags_; }

        void setEventSequence(int32_t event_sequence) { event_sequence_ = event_sequence; }
        int32_t getEventSequence() const { return event_sequence_; }

        void setEventDepth(int32_t event_depth) { event_depth_ = event_depth; }
        int32_t getEventDepth() const { return event_depth_; }

        void decrEventDepth() { event_depth_--; }

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

    	void setUrlStat(std::string_view url_pattern, std::string_view method, int status_code);
    	void sendUrlStat();

    	void setStartTime(std::chrono::system_clock::time_point start_time) { start_time_ = to_milli_seconds(start_time); }
        int64_t getStartTime() const { return start_time_; }

    	void setEndTime() {
	        end_time_ = std::chrono::system_clock::now();
        	elapsed_ = to_milli_seconds(end_time_) - start_time_;
        }
        int32_t getElapsed() const { return elapsed_; }

    	void addSpanEvent(const std::shared_ptr<SpanEventImpl>& se);
    	void finishSpanEvent();
    	std::shared_ptr<SpanEventImpl> topSpanEvent() { return event_stack_.top(); }

        std::vector<std::shared_ptr<SpanEventImpl>>& getFinishedEvents() { return finished_events; }
    	size_t getFinishedEventsCount() const { return finished_events.size(); }
    	void clearFinishedEvents() { finished_events.clear(); }

        std::shared_ptr<PinpointAnnotation> getAnnotations() const { return annotations_; }
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

    	int32_t logging_info_;
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
    	AgentService *agent_;
    };

	class SpanChunk final {
	public:
		SpanChunk(const std::shared_ptr<SpanData>& span_data, bool final);
		~SpanChunk() = default;

		void optimizeSpanEvents();

		std::shared_ptr<SpanData>& getSpanData() { return span_data_; }
		std::vector<std::shared_ptr<SpanEventImpl>>& getSpanEventChunk() { return event_chunk_; }
		int64_t getKeyTime() const { return key_time_; }
		bool isFinal() const { return final_; }

	private:
		std::shared_ptr<SpanData> span_data_;
		std::vector<std::shared_ptr<SpanEventImpl>> event_chunk_;
		bool final_;
		int64_t key_time_;
	};

    class SpanImpl final : public Span {
    public:
        SpanImpl(AgentService* agent, std::string_view operation, std::string_view rpc_point);
        ~SpanImpl() override = default;

    	SpanEventPtr NewSpanEvent(std::string_view operation) override {
    		return NewSpanEvent(operation, DEFAULT_SERVICE_TYPE);
    	}
    	SpanEventPtr NewSpanEvent(std::string_view operation, int32_t service_type) override;
        SpanEventPtr GetSpanEvent() override;
      	void EndSpanEvent() override;
      	void EndSpan() override;
    	SpanPtr NewAsyncSpan(std::string_view async_operation) override;

      	void InjectContext(TraceContextWriter& writer) override;
      	void ExtractContext(TraceContextReader& reader) override;

    	TraceId& GetTraceId() override { return data_->getTraceId(); }
    	int64_t GetSpanId() override { return data_->getSpanId(); }
    	bool IsSampled() override { return true; }
    	AnnotationPtr GetAnnotations() const override { return data_->getAnnotations(); }

    	void SetServiceType(int32_t service_type) override;
    	void SetStartTime(std::chrono::system_clock::time_point start_time) override;
        void SetRemoteAddress(std::string_view address) override;
        void SetEndPoint(std::string_view end_point) override;
    	void SetError(std::string_view error_message) override;
    	void SetError(std::string_view error_name, std::string_view error_message) override;
    	void SetStatusCode(int status) override;
        void SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) override;
    	void RecordHeader(HeaderType which, HeaderReader& reader) override;

    private:
		AgentService *agent_;
    	std::shared_ptr<SpanData> data_;
    	int32_t overflow_;
    	bool finished_;

    	void record_chunk(bool final) const;
    };

}  // namespace pinpoint

