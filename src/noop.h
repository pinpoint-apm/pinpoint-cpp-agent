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
#include "agent_service.h"
#include "url_stat.h"

namespace pinpoint {

    AnnotationPtr noopAnnotation();
    SpanEventPtr noopSpanEvent();
    SpanPtr noopSpan();
    AgentPtr noopAgent();

    class NoopAnnotation final : public Annotation {
    public:
        NoopAnnotation() {}
        ~NoopAnnotation() override {}

        void AppendInt(int32_t key, int32_t i) override {}
        void AppendLong(int32_t key, int64_t l) override {}
        void AppendString(int32_t key, std::string_view s) override {}
        void AppendStringString(int32_t key, std::string_view s1, std::string_view s2) override {}
        void AppendIntStringString(int32_t key, int i, std::string_view s1, std::string_view s2) override {}
        void AppendBytesStringString(int32_t key, std::vector<unsigned char> uid, std::string_view s1, std::string_view s2) override {}
        void AppendLongIntIntByteByteString(int32_t key, int64_t l, int32_t i1, int32_t i2, int32_t b1, int32_t b2, std::string_view s) override {}
    };

    class NoopSpanEvent final : public SpanEvent {
    public:
        NoopSpanEvent() {}
        ~NoopSpanEvent() override {}

        void SetServiceType(int32_t type) override {}
        void SetOperationName(std::string_view operation) override {}
        void SetStartTime(std::chrono::system_clock::time_point start_time) override {}
        void SetDestination(std::string_view dest) override {}
        void SetEndPoint(std::string_view end_point) override {}
        void SetError(std::string_view error_message) override {}
        void SetError(std::string_view error_name, std::string_view error_message) override {}
        void SetError(std::string_view error_name, std::string_view error_message, CallStackReader& reader) override {}
        void SetSqlQuery(std::string_view sql_query, std::string_view args) override {}
        void RecordHeader(HeaderType which, HeaderReader& reader) override {}

        AnnotationPtr GetAnnotations() const override { return noopAnnotation(); }
    };

    class NoopSpan : public Span {
    public:
        NoopSpan() : empty_trace_id{} {}
        ~NoopSpan() override {}

        SpanEventPtr NewSpanEvent(std::string_view operation) override { return noopSpanEvent(); }
        SpanEventPtr NewSpanEvent(std::string_view operation, int32_t service_type) override { return noopSpanEvent(); }
        SpanEventPtr GetSpanEvent() override { return noopSpanEvent(); }
        void EndSpanEvent() override {}
        void EndSpan() override {}
        SpanPtr NewAsyncSpan(std::string_view async_operation) override { return noopSpan(); }

        void InjectContext(TraceContextWriter& writer) override {}
        void ExtractContext(TraceContextReader& reader) override {}

        void SetServiceType(int32_t service_type) override {}
        void SetStartTime(std::chrono::system_clock::time_point start_time) override {}
        void SetRemoteAddress(std::string_view address) override {}
        void SetEndPoint(std::string_view end_point) override {}
        void SetError(std::string_view error_message) override {}
        void SetError(std::string_view error_name, std::string_view error_message) override {}
        void SetStatusCode(int status) override {}
        void SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) override {}
        void RecordHeader(HeaderType which, HeaderReader& reader) override {}

        TraceId& GetTraceId() override { return empty_trace_id; }
        int64_t GetSpanId() override { return 0; }
        bool IsSampled() override { return false; }
        AnnotationPtr GetAnnotations() const override { return noopAnnotation(); }

    protected:
        TraceId empty_trace_id;
    };

    class UnsampledSpan final : public NoopSpan {
    public:
        explicit UnsampledSpan(AgentService *agent);
        ~UnsampledSpan() override {}

        void EndSpan() override;
        void InjectContext(TraceContextWriter& writer) override;
        void SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) override;

        int64_t GetSpanId() override {
            return span_id_;
        }

    private:
        int64_t span_id_;
        int64_t start_time_;
        std::unique_ptr<UrlStat> url_stat_;
        AgentService *agent_;
    };

    class NoopAgent final : public Agent {
    public:
        NoopAgent() {}
        ~NoopAgent() override {}

        SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point) override { return noopSpan(); }
        SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point,
            TraceContextReader& reader) override { return noopSpan(); }
        SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point, std::string_view method,
            TraceContextReader& reader) override { return noopSpan(); }

        bool Enable() override { return false; }
        void Shutdown() override {}
    };

    class NoopTraceContextReader final : public TraceContextReader {
    public:
        NoopTraceContextReader() = default;
        ~NoopTraceContextReader() override = default;

        std::optional<std::string> Get(std::string_view key) const override { return std::nullopt; }
    };

    class Noop {
    public:
        Noop() :
            noop_agent_(std::make_shared<NoopAgent>()),
            noop_span_ (std::make_shared<NoopSpan>()),
            noop_event_(std::make_shared<NoopSpanEvent>()),
            noop_annotation_(std::make_shared<NoopAnnotation>())
        {}

        AgentPtr agent() const { return noop_agent_; }
        SpanPtr span() const { return noop_span_; }
        SpanEventPtr spanEvent() const { return noop_event_; }
        AnnotationPtr annotation() const { return noop_annotation_; }

    private:
        Noop(const Noop&) = delete;
        Noop& operator=(const Noop&) = delete;
        Noop(Noop&&) = delete;
        Noop& operator=(Noop&&) = delete;

        AgentPtr noop_agent_;
        SpanPtr noop_span_;
        SpanEventPtr noop_event_;
        AnnotationPtr noop_annotation_;
    };
}
