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

#ifndef PINPOINT_TRACER_H
#define PINPOINT_TRACER_H

#include <chrono>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <optional>

namespace pinpoint {

	/**
	 * @brief HTTP header names used to propagate Pinpoint trace context.
	 */
	const std::string HEADER_TRACE_ID = "Pinpoint-TraceID";
	const std::string HEADER_SPAN_ID = "Pinpoint-SpanID";
	const std::string HEADER_PARENT_SPAN_ID = "Pinpoint-pSpanID";
	const std::string HEADER_SAMPLED = "Pinpoint-Sampled";
	const std::string HEADER_FLAG = "Pinpoint-Flags";
	const std::string HEADER_PARENT_APP_NAME = "Pinpoint-pAppName";
	const std::string HEADER_PARENT_APP_TYPE = "Pinpoint-pAppType";
	const std::string HEADER_PARENT_APP_NAMESPACE = "Pinpoint-pAppNamespace";
	const std::string HEADER_HOST = "Pinpoint-Host";

	constexpr int32_t ANNOTATION_API = 12;
	constexpr int32_t ANNOTATION_SQL_ID = 20;
	constexpr int32_t ANNOTATION_SQL_UID = 25;
	constexpr int32_t ANNOTATION_EXCEPTION_ID = -52;
	constexpr int32_t ANNOTATION_HTTP_URL = 40;
	constexpr int32_t ANNOTATION_HTTP_STATUS_CODE = 46;
	constexpr int32_t ANNOTATION_HTTP_COOKIE = 45;
	constexpr int32_t ANNOTATION_HTTP_REQUEST_HEADER = 47;
	constexpr int32_t ANNOTATION_HTTP_RESPONSE_HEADER = 55;
	constexpr int32_t ANNOTATION_HTTP_PROXY_HEADER = 300;

	constexpr int32_t APP_TYPE_CPP = 1300;
	constexpr int32_t SERVICE_TYPE_CPP = APP_TYPE_CPP;
	constexpr int32_t SERVICE_TYPE_CPP_FUNC = 1301;
	constexpr int32_t SERVICE_TYPE_CPP_HTTP_CLIENT = 9800;
	constexpr int32_t SERVICE_TYPE_ASYNC = 100;

	constexpr int32_t SERVICE_TYPE_MYSQL_QUERY = 2101;
	constexpr int32_t SERVICE_TYPE_MSSQL_QUERY = 2201;
	constexpr int32_t SERVICE_TYPE_ORACLE_QUERY = 2301;
	constexpr int32_t SERVICE_TYPE_PGSQL_QUERY = 2501;
	constexpr int32_t SERVICE_TYPE_CASSANDRA_QUERY = 2601;
	constexpr int32_t SERVICE_TYPE_MONGODB_QUERY = 2651;

	constexpr int32_t SERVICE_TYPE_MEMCACHED = 8050;
	constexpr int32_t SERVICE_TYPE_REDIS = 8203;
	constexpr int32_t SERVICE_TYPE_KFAKA = 8660;
	constexpr int32_t SERVICE_TYPE_HBASE = 8800;

	constexpr int32_t SERVICE_TYPE_GRPC_CLIENT = 9160;
	constexpr int32_t SERVICE_TYPE_GRPC_SERVER = 1130;

	constexpr int32_t API_TYPE_DEFAULT = 0;
	constexpr int32_t API_TYPE_WEB_REQUEST = 100;
	constexpr int32_t API_TYPE_INVOCATION = 200;

	constexpr int32_t NONE_ASYNC_ID = 0;

	/**
	 * @brief Represents a distributed trace identifier consisting of agent, start time and sequence.
	 */
	struct TraceId {
		/// Agent identifier that issued the trace.
		std::string AgentId;
		/// Epoch time (milliseconds) when the agent started.
		int64_t StartTime;
		/// Sequence number that disambiguates traces created at the same start time.
		int64_t Sequence;

		/**
		 * @brief Serializes the trace identifier to the wire format (`agentId^startTime^sequence`).
		 */
		std::string ToString() const {
			std::ostringstream out;
			out << AgentId << "^" << StartTime << "^" << Sequence;
			return out.str();

		}
	};

	/**
	 * @brief Read-only accessor for inbound propagation carriers.
	 */
	class TraceContextReader {
    public:
    	virtual ~TraceContextReader() = default;
		/**
		 * @brief Reads a key value from the propagation carrier.
		 *
		 * @param key Case-sensitive header name or key.
		 * @return Value if present.
		 */
        virtual std::optional<std::string> Get(std::string_view key) const = 0;
    };

	/**
	 * @brief Write-only accessor for outbound propagation carriers.
	 */
	class TraceContextWriter {
    public:
    	virtual ~TraceContextWriter() = default;
		/**
		 * @brief Writes a key/value pair into the propagation carrier.
		 *
		 * @param key Case-insensitive header name or key.
		 * @param value Value to set (implementation may copy or reference).
		 */
        virtual void Set(std::string_view key, std::string_view value) = 0;
    };

	/**
	 * @brief Enumerates logical header groups that can be recorded on spans.
	 */
	enum HeaderType {
		HTTP_REQUEST = 0, HTTP_RESPONSE, HTTP_COOKIE
	};

	/**
	 * @brief Interface used to iterate through headers without exposing container details.
	 */
	class HeaderReader : public TraceContextReader {
	public:
		virtual ~HeaderReader() override = default;
		/**
		 * @brief Looks up a single header value by key.
		 */
		virtual std::optional<std::string> Get(std::string_view key) const override = 0;
		/**
		 * @brief Iterates through all headers invoking the callback for each entry.
		 *
		 * @param callback Return false to stop iteration early.
		 */
		virtual void ForEach(std::function<bool(std::string_view key, std::string_view val)> callback) const = 0;
	};

	class HeaderReaderWriter : public HeaderReader, public TraceContextWriter {
	public:
		virtual ~HeaderReaderWriter() override = default;
		/**
		 * @brief Looks up a single header value by key.
		 */
		virtual std::optional<std::string> Get(std::string_view key) const override = 0;
		/**
		 * @brief Iterates through all headers invoking the callback for each entry.
		 *
		 * @param callback Return false to stop iteration early.
		 */
		virtual void ForEach(std::function<bool(std::string_view key, std::string_view val)> callback) const override = 0;
		/**
		 * @brief Writes a key/value pair into the propagation carrier.
		 */
		virtual void Set(std::string_view key, std::string_view value) override = 0;
	};

	/**
	 * @brief Interface used to enumerates frames stored inside a call stack.
	 */
	class CallStackReader {
	public:
		virtual ~CallStackReader() = default;
		/**
		 * @brief Iterates through all frames in the call stack.
		 *
		 * @param callback Invoked with module, function, file and line for each frame.
		 */
		virtual void ForEach(std::function<void(std::string_view module, std::string_view function, std::string_view file, int line)> callback) const = 0;
	};
	
	/**
	 * @brief Abstract container for span annotations.
	 */
    class Annotation {
    public:
        virtual ~Annotation() = default;

		/// @brief Records an integer annotation.
        virtual void AppendInt(int32_t key, int32_t i) = 0;
		/// @brief Records a long annotation.
        virtual void AppendLong(int32_t key, int64_t l) = 0;
		/// @brief Records a string annotation.
        virtual void AppendString(int32_t key, std::string_view s) = 0;
		/// @brief Records an annotation with two string values.
        virtual void AppendStringString(int32_t key, std::string_view s1, std::string_view s2) = 0;
		/// @brief Records an annotation with an integer and two string values.
        virtual void AppendIntStringString(int32_t key, int i, std::string_view s1, std::string_view s2) = 0;
		/// @brief Records an annotation with binary payload and two strings.
        virtual void AppendBytesStringString(int32_t key, std::vector<unsigned char> uid, std::string_view s1, std::string_view s2) = 0;
		/// @brief Records a detailed network annotation.
        virtual void AppendLongIntIntByteByteString(int32_t key, int64_t l, int32_t i1, int32_t i2, int32_t b1, int32_t b2, std::string_view s) = 0;
    };

	using AnnotationPtr = std::shared_ptr<Annotation>;

	/**
	 * @brief Interface describing a span event recorded within a span.
	 */
    class SpanEvent {
    public:
        virtual ~SpanEvent() = default;

		/// @brief Sets the service type for the span event.
        virtual void SetServiceType(int32_t type) = 0;
		/// @brief Sets the logical operation recorded by the event.
        virtual void SetOperationName(std::string_view operation) = 0;
		/// @brief Records the event's start timestamp.
		virtual void SetStartTime(std::chrono::system_clock::time_point start_time) = 0;
		/// @brief Records the destination identifier (for RPCs).
        virtual void SetDestination(std::string_view dest) = 0;
		/// @brief Records the remote endpoint.
        virtual void SetEndPoint(std::string_view end_point) = 0;
		/// @brief Stores an error message.
        virtual void SetError(std::string_view error_message) = 0;
		/// @brief Stores a named error message.
        virtual void SetError(std::string_view error_name, std::string_view error_message) = 0;
		/// @brief Stores an error message along with call stack details.
        virtual void SetError(std::string_view error_name, std::string_view error_message, CallStackReader& reader) = 0;
		/// @brief Records a SQL query and bound parameters.
		virtual void SetSqlQuery(std::string_view sql_query, std::string_view args) = 0;
		/// @brief Records HTTP headers into the event.
        virtual void RecordHeader(HeaderType which, HeaderReader& reader) = 0;

		/// @brief Returns the mutable annotation container.
        virtual AnnotationPtr GetAnnotations() const = 0;
    };

	using SpanEventPtr = std::shared_ptr<SpanEvent>;

	class Span;
	using SpanPtr = std::shared_ptr<Span>;

	/**
	 * @brief Interface implemented by concrete spans managed by the Pinpoint agent.
	 */
	class Span {
	public:
		virtual ~Span() = default;

		/// @brief Creates a new span event using the default service type.
		virtual SpanEventPtr NewSpanEvent(std::string_view operation) = 0;
		/// @brief Creates a new span event using the specified service type.
		virtual SpanEventPtr NewSpanEvent(std::string_view operation, int32_t service_type) = 0;
		/// @brief Returns the active span event.
		virtual SpanEventPtr GetSpanEvent() = 0;
		/// @brief Finalizes the current span event.
		virtual void EndSpanEvent() = 0;
		/// @brief Completes the span and flushes recorded data.
		virtual void EndSpan() = 0;
		/// @brief Creates an asynchronous child span for background work.
		virtual SpanPtr NewAsyncSpan(std::string_view async_operation) = 0;

		/// @brief Injects the span context into an outbound carrier.
		virtual void InjectContext(TraceContextWriter& writer) = 0;
		/// @brief Extracts a span context from an inbound carrier.
		virtual void ExtractContext(TraceContextReader& reader) = 0;

		/// @brief Returns the trace identifier for the span.
		virtual TraceId& GetTraceId() = 0;
		/// @brief Returns the span identifier.
		virtual int64_t GetSpanId() = 0;
		/// @brief Indicates whether the span is sampled.
		virtual bool IsSampled() = 0;

		/// @brief Sets the span service type.
		virtual void SetServiceType(int32_t service_type) = 0;
		/// @brief Records the span start time.
		virtual void SetStartTime(std::chrono::system_clock::time_point start_time) = 0;
		/// @brief Records the remote address.
		virtual void SetRemoteAddress(std::string_view address) = 0;
		/// @brief Records the endpoint served by the span.
		virtual void SetEndPoint(std::string_view end_point) = 0;
		/// @brief Records an error message at the span level.
		virtual void SetError(std::string_view error_message) = 0;
		/// @brief Records a named error message at the span level.
		virtual void SetError(std::string_view error_name, std::string_view error_message) = 0;
		/// @brief Records the HTTP status code for the span.
		virtual void SetStatusCode(int status) = 0;
		/// @brief Records URL statistics for the span.
		virtual void SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) = 0;
		/// @brief Records the logging flag and injects the span context into a logger.
		virtual void SetLogging(TraceContextWriter& writer) = 0;
		/// @brief Records HTTP headers for the span.
		virtual void RecordHeader(HeaderType which, HeaderReader& reader) = 0;

		/// @brief Returns the span annotations.
		virtual AnnotationPtr GetAnnotations() const = 0;
	};

	/**
	 * @brief Interface exposed to application code for creating spans.
	 */
  	class Agent {
   	public:
   		virtual ~Agent() = default;

		/**
		 * @brief Creates a new span for an outbound RPC/operation.
		 *
		 * @param operation Logical name of the operation.
		 * @param rpc_point RPC endpoint or destination.
		 */
		virtual SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point) = 0;
      	/**
      	 * @brief Creates a new span using context extracted from inbound carrier.
      	 *
      	 * @param operation Logical name of the operation.
      	 * @param rpc_point RPC endpoint or destination.
      	 * @param reader Trace context carrier reader.
      	 */
      	virtual SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point, TraceContextReader& reader) = 0;
      	/**
      	 * @brief Creates a new span recording HTTP method information.
      	 *
      	 * @param operation Logical name of the operation.
      	 * @param rpc_point RPC endpoint or destination.
      	 * @param method HTTP method (GET, POST, ...).
      	 * @param reader Trace context carrier reader.
      	 */
      	virtual SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point, std::string_view method, TraceContextReader& reader) = 0;
      	/// @brief Returns whether the agent is enabled and sampling.
      	virtual bool Enable() = 0;
      	/// @brief Initiates a graceful shutdown of the agent.
      	virtual void Shutdown() = 0;
  	};

	using AgentPtr = std::shared_ptr<Agent>;

	/// @brief Set the configuration file path used by the global agent.
	void SetConfigFilePath(std::string_view config_file_path);
	/// @brief Inject raw configuration YAML directly.
	void SetConfigString(std::string_view config_string);

	/// @brief Creates an agent using the global configuration.
	AgentPtr CreateAgent();
	/// @brief Creates an agent overriding the default application type.
	AgentPtr CreateAgent(int32_t app_type);
	/// @brief Returns the singleton global agent instance.
	AgentPtr GlobalAgent();

	namespace helper {
		/**
		 * @brief Traces a HTTP server request.
		 *
		 * @param span The span to trace.
		 * @param remote_addr The remote address.
		 * @param endpoint The endpoint.
		 * @param request_reader The request reader.
		 */
		void TraceHttpServerRequest(SpanPtr span, std::string_view remote_addr, std::string_view endpoint, HeaderReader& request_reader);

		/**
		 * @brief Traces a HTTP server request.
		 *
		 * @param span The span to trace.
		 * @param remote_addr The remote address.
		 * @param endpoint The endpoint.
		 * @param request_reader The request reader.
		 * @param cookie_reader The cookie reader.
		 */
		void TraceHttpServerRequest(SpanPtr span, std::string_view remote_addr, std::string_view endpoint, HeaderReader& request_reader, HeaderReader& cookie_reader);

		/**
		 * @brief Traces a HTTP server response.
		 *
		 * @param span The span to trace.
		 * @param url_pattern The URL pattern.
		 * @param method The method.
		 * @param status_code The status code.
		 */
		void TraceHttpServerResponse(SpanPtr span, std::string_view url_pattern, std::string_view method, int status_code, HeaderReader& response_reader);

		/**
		 * @brief Traces a HTTP client request.
		 *
		 * @param span_event The span event to trace.
		 * @param host The host.
		 * @param url The URL.
		 * @param request_reader The request reader.
		 */
		 void TraceHttpClientRequest(SpanEventPtr span_event, std::string_view host, std::string_view url, HeaderReader& request_reader);

		 /**
		 * @brief Traces a HTTP client request.
		 *
		 * @param span_event The span event to trace.
		 * @param host The host.
		 * @param url The URL.
		 * @param request_reader The request reader.
		 * @param cookie_reader The cookie reader.
		 */
		 void TraceHttpClientRequest(SpanEventPtr span_event, std::string_view host, std::string_view url, HeaderReader& request_reader, HeaderReader& cookie_reader);

		 /**
		 * @brief Traces a HTTP client response.
		 *
		 * @param span_event The span event to trace.
		 * @param status_code The status code.
		 * @param response_reader The response reader.
		 */
		void TraceHttpClientResponse(SpanEventPtr span_event, int status_code, HeaderReader& response_reader);

		// RAII helper to manage span events.
		class ScopedSpanEvent {
		public:
			explicit ScopedSpanEvent(const SpanPtr& span, std::string_view operation) : span_(span) {
				event_ = span_->NewSpanEvent(operation, SERVICE_TYPE_CPP_FUNC);
			}
			explicit ScopedSpanEvent(const SpanPtr& span, std::string_view operation, int32_t service_type) : span_(span) {
				event_ = span_->NewSpanEvent(operation, service_type);
			}
		
			~ScopedSpanEvent() {
				span_->EndSpanEvent();
			}
		
			SpanEventPtr operator->() const { return event_; }
			SpanEventPtr value() const { return event_; }
		
		private:
			SpanPtr span_;
			SpanEventPtr event_;
		};
   
	};
	
}  // namespace pinpoint

#endif //PINPOINT_TRACER_H
