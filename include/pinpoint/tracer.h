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
	constexpr int32_t ANNOTATION_HTTP_URL = 40;
	constexpr int32_t ANNOTATION_HTTP_STATUS_CODE = 46;
	constexpr int32_t ANNOTATION_HTTP_COOKIE = 45;
	constexpr int32_t ANNOTATION_HTTP_REQUEST_HEADER = 47;
	constexpr int32_t ANNOTATION_HTTP_RESPONSE_HEADER = 55;

	constexpr int32_t APP_TYPE_CPP = 1300;
	constexpr int32_t SERVICE_TYPE_CPP_FUNC = 1301;
	constexpr int32_t SERVICE_TYPE_CPP_HTTP_CLIENT = 9800;
	constexpr int32_t SERVICE_TYPE_ASYNC = 100;
	constexpr int32_t DEFAULT_APP_TYPE = APP_TYPE_CPP;
	constexpr int32_t DEFAULT_SERVICE_TYPE = SERVICE_TYPE_CPP_FUNC;

	constexpr int32_t API_TYPE_DEFAULT = 0;
	constexpr int32_t API_TYPE_WEB_REQUEST = 100;
	constexpr int32_t API_TYPE_INVOCATION = 200;

	constexpr int32_t NONE_ASYNC_ID = 0;

	struct TraceId {
		std::string AgentId;
		int64_t StartTime;
		int64_t Sequence;

		std::string ToString() const {
			std::ostringstream out;
			out << AgentId << "^" << StartTime << "^" << Sequence;
			return out.str();

		}
	};

	class TraceContextReader {
    public:
    	virtual ~TraceContextReader() = default;
        virtual std::optional<std::string> Get(std::string_view key) const = 0;
    };

	class TraceContextWriter {
    public:
    	virtual ~TraceContextWriter() = default;
        virtual void Set(std::string_view key, std::string_view value) = 0;
    };

	enum HeaderType {
		HTTP_REQUEST = 0, HTTP_RESPONSE, HTTP_COOKIE
	};

	class HeaderReader {
	public:
		virtual ~HeaderReader() = default;
		virtual std::optional<std::string> Get(std::string_view key) const = 0;
		virtual void ForEach(std::function<bool(std::string_view key, std::string_view val)> callback) const = 0;
	};

    class Annotation {
    public:
        virtual ~Annotation() = default;

        virtual void AppendInt(int32_t key, int i) = 0;
        virtual void AppendString(int32_t key, std::string_view s) = 0;
        virtual void AppendStringString(int32_t key, std::string_view s1, std::string_view s2) = 0;
        virtual void AppendIntStringString(int32_t key, int i, std::string_view s1, std::string_view s2) = 0;
        virtual void AppendLongIntIntByteByteString(int32_t key, int64_t l, int32_t i1, int32_t i2, int32_t b1, int32_t b2, std::string_view s) = 0;
    };

	using AnnotationPtr = std::shared_ptr<Annotation>;

    class SpanEvent {
    public:
        virtual ~SpanEvent() = default;

        virtual void SetServiceType(int32_t type) = 0;
        virtual void SetOperationName(std::string_view operation) = 0;
		virtual void SetStartTime(std::chrono::system_clock::time_point start_time) = 0;
        virtual void SetDestination(std::string_view dest) = 0;
        virtual void SetEndPoint(std::string_view end_point) = 0;
        virtual void SetError(std::string_view error_message) = 0;
        virtual void SetError(std::string_view error_name, std::string_view error_message) = 0;
		virtual void SetSqlQuery(std::string_view sql_query, std::string_view args) = 0;
        virtual void RecordHeader(HeaderType which, HeaderReader& reader) = 0;

        virtual AnnotationPtr GetAnnotations() const = 0;
    };

	using SpanEventPtr = std::shared_ptr<SpanEvent>;

	class Span;
	using SpanPtr = std::shared_ptr<Span>;

	class Span {
	public:
		virtual ~Span() = default;

		virtual SpanEventPtr NewSpanEvent(std::string_view operation) = 0;
		virtual SpanEventPtr NewSpanEvent(std::string_view operation, int32_t service_type) = 0;
		virtual SpanEventPtr GetSpanEvent() = 0;
		virtual void EndSpanEvent() = 0;
		virtual void EndSpan() = 0;
		virtual SpanPtr NewAsyncSpan(std::string_view async_operation) = 0;

		virtual void InjectContext(TraceContextWriter& writer) = 0;
		virtual void ExtractContext(TraceContextReader& reader) = 0;

		virtual TraceId& GetTraceId() = 0;
		virtual int64_t GetSpanId() = 0;
		virtual bool IsSampled() = 0;

		virtual void SetServiceType(int32_t service_type) = 0;
		virtual void SetStartTime(std::chrono::system_clock::time_point start_time) = 0;
		virtual void SetRemoteAddress(std::string_view address) = 0;
		virtual void SetEndPoint(std::string_view end_point) = 0;
		virtual void SetError(std::string_view error_message) = 0;
		virtual void SetError(std::string_view error_name, std::string_view error_message) = 0;
		virtual void SetStatusCode(int status) = 0;
		virtual void SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) = 0;
		virtual void RecordHeader(HeaderType which, HeaderReader& reader) = 0;

		virtual AnnotationPtr GetAnnotations() const = 0;
	};

  	class Agent {
   	public:
   		virtual ~Agent() = default;

		virtual SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point) = 0;
      	virtual SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point, TraceContextReader& reader) = 0;
      	virtual SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point, std::string_view method, TraceContextReader& reader) = 0;
      	virtual bool Enable() = 0;
      	virtual void Shutdown() = 0;
  	};

	using AgentPtr = std::shared_ptr<Agent>;

	void SetConfigFilePath(std::string_view config_file_path);
	void SetConfigString(std::string_view config_string);

	AgentPtr CreateAgent();
	AgentPtr CreateAgent(int32_t app_type);
	AgentPtr GlobalAgent();

}  // namespace pinpoint

#endif //PINPOINT_TRACER_H
