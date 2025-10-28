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

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <map>
#include <functional>

#include "../src/noop.h"
#include "../src/agent_service.h"
#include "../src/config.h"
#include "../src/span.h"
#include "../src/url_stat.h"
#include "../include/pinpoint/tracer.h"

namespace pinpoint {

class MockAgentService : public AgentService {
public:
    MockAgentService() : is_exiting_(false), start_time_(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()), trace_id_counter_(0) {
        config_.span.event_chunk_size = 10; // Set a reasonable default for testing
        config_.span.max_event_depth = 32;
        config_.span.queue_size = 1024;
        config_.http.url_stat.enable = true; // Enable URL stat for relevant tests
        config_.http.url_stat.limit = 1024;
        config_.http.url_stat.path_depth = 3;
    }

    bool isExiting() const override { return is_exiting_; }
    void setExiting(bool exiting) { is_exiting_ = exiting; }

    std::string_view getAppName() const override { return "mock-app"; }
    int32_t getAppType() const override { return 1300; }
    std::string_view getAgentId() const override { return "mock-agent"; }
    std::string_view getAgentName() const override { return "mock-agent-name"; }
    const Config& getConfig() const override { return config_; }
    int64_t getStartTime() const override { return start_time_; }

    TraceId generateTraceId() override {
        return TraceId{"mock-agent", start_time_, trace_id_counter_++};
    }
    void recordSpan(std::unique_ptr<SpanChunk> span) const override {
        recorded_spans_.push_back(std::move(span));
    }
    void recordUrlStat(std::unique_ptr<UrlStat> stat) const override {
        recorded_url_stats_++;
        last_url_stat_url_ = stat->url_pattern_;
        last_url_stat_method_ = stat->method_;
        last_url_stat_status_code_ = stat->status_code_;
    }
    void recordException(SpanData* span_data) const override {
        recorded_exceptions_++;
    }
    void recordStats(StatsType stats) const override {
        recorded_stats_calls_++;
        last_stats_type_ = stats;
    }

    int32_t cacheApi(std::string_view api_str, int32_t api_type) const override {
        if (cached_apis_.find(std::string(api_str)) == cached_apis_.end()) {
            cached_apis_[std::string(api_str)] = api_id_counter_++;
        }
        return cached_apis_[std::string(api_str)];
    }
    void removeCacheApi(const ApiMeta& api_meta) const override {}
    int32_t cacheError(std::string_view error_name) const override {
        if (cached_errors_.find(std::string(error_name)) == cached_errors_.end()) {
            cached_errors_[std::string(error_name)] = error_id_counter_++;
        }
        return cached_errors_[std::string(error_name)];
    }
    void removeCacheError(const StringMeta& error_meta) const override {}

    int32_t cacheSql(std::string_view sql_query) const override {
        if (cached_sqls_.find(std::string(sql_query)) == cached_sqls_.end()) {
            cached_sqls_[std::string(sql_query)] = sql_id_counter_++;
        }
        return cached_sqls_[std::string(sql_query)];
    }
    void removeCacheSql(const StringMeta& sql_meta) const override {}

    std::vector<unsigned char> cacheSqlUid(std::string_view sql) const override {
        // Mock implementation - return test uid
        return {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    }

    void removeCacheSqlUid(const SqlUidMeta& sql_uid_meta) const override {
        // Mock implementation
    }

    bool isStatusFail(int status) const override { return status >= 400; }
    void recordServerHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const override {
        recorded_server_headers_++;
    }
    void recordClientHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const override {
        recorded_client_headers_++;
    }

    // Test-specific accessors
    mutable int recorded_stats_calls_ = 0;
    mutable StatsType last_stats_type_ = AGENT_STATS;
    mutable int recorded_url_stats_ = 0;
    mutable int recorded_exceptions_ = 0;
    mutable std::string last_url_stat_url_;
    mutable std::string last_url_stat_method_;
    mutable int last_url_stat_status_code_ = 0;
    mutable int recorded_server_headers_ = 0;
    mutable int recorded_client_headers_ = 0;
    mutable std::vector<std::unique_ptr<SpanChunk>> recorded_spans_;
    mutable std::map<std::string, int32_t> cached_apis_;
    mutable std::map<std::string, int32_t> cached_errors_;
    mutable std::map<std::string, int32_t> cached_sqls_;
    mutable int32_t api_id_counter_ = 100;
    mutable int32_t error_id_counter_ = 200;
    mutable int32_t sql_id_counter_ = 300;

private:
    bool is_exiting_;
    int64_t start_time_;
    int64_t trace_id_counter_;
    Config config_;
};

class MockTraceContextWriter : public TraceContextWriter {
public:
    MockTraceContextWriter() = default;
    void Set(std::string_view key, std::string_view value) override {
        context_[std::string(key)] = std::string(value);
    }
    std::optional<std::string> Get(std::string_view key) const {
        auto it = context_.find(std::string(key));
        if (it != context_.end()) {
            return it->second;
        }
        return std::nullopt;
    }
private:
    std::map<std::string, std::string> context_;
};

class MockTraceContextReader : public TraceContextReader {
public:
    MockTraceContextReader() = default;
    std::optional<std::string> Get(std::string_view key) const override {
        auto it = context_.find(std::string(key));
        if (it != context_.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    void SetContext(std::string_view key, std::string_view value) {
        context_[std::string(key)] = std::string(value);
    }
private:
    std::map<std::string, std::string> context_;
};

class MockHeaderReader : public HeaderReader {
public:
    MockHeaderReader() = default;

    std::optional<std::string> Get(std::string_view key) const override {
        auto it = headers_.find(std::string(key));
        if (it != headers_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    void ForEach(std::function<bool(std::string_view key, std::string_view val)> callback) const override {
        for (const auto& pair : headers_) {
            if (!callback(pair.first, pair.second)) {
                break;
            }
        }
    }

    void SetHeader(std::string_view key, std::string_view value) {
        headers_[std::string(key)] = std::string(value);
    }

private:
    std::map<std::string, std::string> headers_;
};

class NoopTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_agent_service_ = std::make_unique<MockAgentService>();
        mock_agent_service_->setExiting(false);
    }

    void TearDown() override {
        mock_agent_service_.reset();
    }

    std::unique_ptr<MockAgentService> mock_agent_service_;
};

// UnsampledSpan Tests

TEST_F(NoopTest, UnsampledSpanConstructorTest) {
    UnsampledSpan span(mock_agent_service_.get());

    EXPECT_NE(span.GetSpanId(), 0) << "UnsampledSpan should have a non-zero span ID";
    EXPECT_FALSE(span.IsSampled()) << "UnsampledSpan should not be sampled";
}

TEST_F(NoopTest, UnsampledSpanUniqueSpanIdsTest) {
    UnsampledSpan span1(mock_agent_service_.get());
    UnsampledSpan span2(mock_agent_service_.get());

    EXPECT_NE(span1.GetSpanId(), span2.GetSpanId()) << "Different UnsampledSpan instances should have unique span IDs";
}

TEST_F(NoopTest, UnsampledSpanInjectContextTest) {
    UnsampledSpan span(mock_agent_service_.get());
    MockTraceContextWriter writer;

    span.InjectContext(writer);

    auto sampled_value = writer.Get(HEADER_SAMPLED);
    EXPECT_TRUE(sampled_value.has_value()) << "Sampled header should be set";
    EXPECT_EQ(sampled_value.value(), "s0") << "Sampled header should be 's0' for unsampled span";
}

TEST_F(NoopTest, UnsampledSpanSetUrlStatTest) {
    UnsampledSpan span(mock_agent_service_.get());

    span.SetUrlStat("/api/users", "GET", 200);

    // URL stat will be recorded only when EndSpan is called
    EXPECT_EQ(mock_agent_service_->recorded_url_stats_, 0) << "URL stat should not be recorded until EndSpan is called";
}

TEST_F(NoopTest, UnsampledSpanEndSpanWithoutUrlStatTest) {
    UnsampledSpan span(mock_agent_service_.get());

    // Give some time for span to exist
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    span.EndSpan();

    EXPECT_EQ(mock_agent_service_->recorded_url_stats_, 0) << "No URL stat should be recorded when not set";
}

TEST_F(NoopTest, UnsampledSpanEndSpanWithUrlStatTest) {
    UnsampledSpan span(mock_agent_service_.get());

    span.SetUrlStat("/api/users", "GET", 200);

    // Give some time for span to exist
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    span.EndSpan();

    EXPECT_EQ(mock_agent_service_->recorded_url_stats_, 1) << "URL stat should be recorded when EndSpan is called";
    EXPECT_EQ(mock_agent_service_->last_url_stat_url_, "/api/users") << "URL should match";
    EXPECT_EQ(mock_agent_service_->last_url_stat_method_, "GET") << "Method should match";
    EXPECT_EQ(mock_agent_service_->last_url_stat_status_code_, 200) << "Status code should match";
}

TEST_F(NoopTest, UnsampledSpanCompleteWorkflowTest) {
    {
        UnsampledSpan span(mock_agent_service_.get());

        span.SetUrlStat("/api/orders", "POST", 201);

        MockTraceContextWriter writer;
        span.InjectContext(writer);

        auto sampled_value = writer.Get(HEADER_SAMPLED);
        EXPECT_EQ(sampled_value.value(), "s0") << "Context should indicate unsampled";

        // Give some time for span to exist
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        span.EndSpan();
    }

    EXPECT_EQ(mock_agent_service_->recorded_url_stats_, 1) << "URL stat should be recorded";
    EXPECT_EQ(mock_agent_service_->last_url_stat_url_, "/api/orders") << "URL should match";
    EXPECT_EQ(mock_agent_service_->last_url_stat_method_, "POST") << "Method should match";
    EXPECT_EQ(mock_agent_service_->last_url_stat_status_code_, 201) << "Status code should match";
}

TEST_F(NoopTest, UnsampledSpanMultipleUrlStatCallsTest) {
    UnsampledSpan span(mock_agent_service_.get());

    // First call should set the URL stat
    span.SetUrlStat("/api/users", "GET", 200);
    
    // Second call should replace the first one
    span.SetUrlStat("/api/orders", "POST", 201);

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    span.EndSpan();

    EXPECT_EQ(mock_agent_service_->recorded_url_stats_, 1) << "Only one URL stat should be recorded";
    EXPECT_EQ(mock_agent_service_->last_url_stat_url_, "/api/orders") << "Last URL should be recorded";
    EXPECT_EQ(mock_agent_service_->last_url_stat_method_, "POST") << "Last method should be recorded";
    EXPECT_EQ(mock_agent_service_->last_url_stat_status_code_, 201) << "Last status code should be recorded";
}

TEST_F(NoopTest, UnsampledSpanInheritedNoopBehaviorTest) {
    UnsampledSpan span(mock_agent_service_.get());

    // Test inherited NoopSpan behavior - these should all be no-ops
    auto span_event = span.NewSpanEvent("test-operation");
    EXPECT_NE(span_event, nullptr) << "NewSpanEvent should return a valid SpanEvent";

    auto span_event_with_type = span.NewSpanEvent("test-operation", 1000);
    EXPECT_NE(span_event_with_type, nullptr) << "NewSpanEvent with service type should return a valid SpanEvent";

    auto current_event = span.GetSpanEvent();
    EXPECT_NE(current_event, nullptr) << "GetSpanEvent should return a valid SpanEvent";

    // These should not throw exceptions
    span.EndSpanEvent();
    span.SetServiceType(1000);
    span.SetStartTime(std::chrono::system_clock::now());
    span.SetRemoteAddress("192.168.1.1");
    span.SetEndPoint("http://api.example.com");
    span.SetError("Test error");
    span.SetError("TestError", "Test error message");
    span.SetStatusCode(500);

    auto async_span = span.NewAsyncSpan("async-operation");
    EXPECT_NE(async_span, nullptr) << "NewAsyncSpan should return a valid Span";

    MockTraceContextWriter writer;
    span.InjectContext(writer); // This should work (overridden method)

    // ExtractContext should be no-op (inherited behavior)
    MockTraceContextReader reader;
    span.ExtractContext(reader);

    auto trace_id = span.GetTraceId();
    EXPECT_NE(&trace_id, nullptr) << "GetTraceId should return a valid reference";

    auto annotations = span.GetAnnotations();
    EXPECT_NE(annotations, nullptr) << "GetAnnotations should return a valid Annotation";
}

// NoopSpan Tests

TEST_F(NoopTest, NoopSpanBasicBehaviorTest) {
    NoopSpan span;

    EXPECT_EQ(span.GetSpanId(), 0) << "NoopSpan should return 0 for span ID";
    EXPECT_FALSE(span.IsSampled()) << "NoopSpan should not be sampled";

    auto trace_id = span.GetTraceId();
    EXPECT_NE(&trace_id, nullptr) << "GetTraceId should return a valid reference";

    auto annotations = span.GetAnnotations();
    EXPECT_NE(annotations, nullptr) << "GetAnnotations should return a valid Annotation";
}

TEST_F(NoopTest, NoopSpanAllMethodsTest) {
    NoopSpan span;

    // All these should be no-ops and not throw exceptions
    auto span_event = span.NewSpanEvent("test-operation");
    EXPECT_NE(span_event, nullptr) << "NewSpanEvent should return a valid SpanEvent";

    auto span_event_with_type = span.NewSpanEvent("test-operation", 1000);
    EXPECT_NE(span_event_with_type, nullptr) << "NewSpanEvent with service type should return a valid SpanEvent";

    auto current_event = span.GetSpanEvent();
    EXPECT_NE(current_event, nullptr) << "GetSpanEvent should return a valid SpanEvent";

    span.EndSpanEvent();
    span.EndSpan();

    auto async_span = span.NewAsyncSpan("async-operation");
    EXPECT_NE(async_span, nullptr) << "NewAsyncSpan should return a valid Span";

    MockTraceContextWriter writer;
    span.InjectContext(writer);

    MockTraceContextReader reader;
    span.ExtractContext(reader);

    span.SetServiceType(1000);
    span.SetStartTime(std::chrono::system_clock::now());
    span.SetRemoteAddress("192.168.1.1");
    span.SetEndPoint("http://api.example.com");
    span.SetError("Test error");
    span.SetError("TestError", "Test error message");
    span.SetStatusCode(500);
    span.SetUrlStat("/api/test", "GET", 200);

    SUCCEED() << "All NoopSpan methods should execute without throwing exceptions";
}

// NoopSpanEvent Tests

TEST_F(NoopTest, NoopSpanEventBasicBehaviorTest) {
    NoopSpanEvent span_event;

    auto annotations = span_event.GetAnnotations();
    EXPECT_NE(annotations, nullptr) << "GetAnnotations should return a valid Annotation";
}

TEST_F(NoopTest, NoopSpanEventAllMethodsTest) {
    NoopSpanEvent span_event;

    // All these should be no-ops and not throw exceptions
    span_event.SetServiceType(1000);
    span_event.SetOperationName("test-operation");
    span_event.SetStartTime(std::chrono::system_clock::now());
    span_event.SetDestination("test-destination");
    span_event.SetEndPoint("http://api.example.com");
    span_event.SetError("Test error");
    span_event.SetError("TestError", "Test error message");

    MockHeaderReader reader;
    span_event.RecordHeader(HTTP_REQUEST, reader);

    SUCCEED() << "All NoopSpanEvent methods should execute without throwing exceptions";
}

// NoopAnnotation Tests

TEST_F(NoopTest, NoopAnnotationAllMethodsTest) {
    NoopAnnotation annotation;

    // All these should be no-ops and not throw exceptions
    annotation.AppendInt(1, 42);
    annotation.AppendString(2, "test-string");
    annotation.AppendStringString(3, "key", "value");
    annotation.AppendIntStringString(4, 100, "key", "value");
    annotation.AppendLongIntIntByteByteString(5, 123456789L, 10, 20, 30, 40, "test");

    SUCCEED() << "All NoopAnnotation methods should execute without throwing exceptions";
}

// Global noop function tests

TEST_F(NoopTest, GlobalNoopFunctionsTest) {
    auto annotation = noopAnnotation();
    EXPECT_NE(annotation, nullptr) << "noopAnnotation should return a valid Annotation";

    auto span_event = noopSpanEvent();
    EXPECT_NE(span_event, nullptr) << "noopSpanEvent should return a valid SpanEvent";

    auto span = noopSpan();
    EXPECT_NE(span, nullptr) << "noopSpan should return a valid Span";

    auto agent = noopAgent();
    EXPECT_NE(agent, nullptr) << "noopAgent should return a valid Agent";

    // Test that the same instances are returned (singleton behavior)
    auto annotation2 = noopAnnotation();
    auto span_event2 = noopSpanEvent();
    auto span2 = noopSpan();
    auto agent2 = noopAgent();

    EXPECT_EQ(annotation.get(), annotation2.get()) << "noopAnnotation should return the same instance";
    EXPECT_EQ(span_event.get(), span_event2.get()) << "noopSpanEvent should return the same instance";
    EXPECT_EQ(span.get(), span2.get()) << "noopSpan should return the same instance";
    EXPECT_EQ(agent.get(), agent2.get()) << "noopAgent should return the same instance";
}

// NoopAgent Tests

TEST_F(NoopTest, NoopAgentBasicBehaviorTest) {
    NoopAgent agent;

    EXPECT_FALSE(agent.Enable()) << "NoopAgent should not be enabled";

    auto span1 = agent.NewSpan("operation", "rpc");
    EXPECT_NE(span1, nullptr) << "NewSpan should return a valid Span";

    MockTraceContextReader reader;
    auto span2 = agent.NewSpan("operation", "rpc", reader);
    EXPECT_NE(span2, nullptr) << "NewSpan with reader should return a valid Span";

    auto span3 = agent.NewSpan("operation", "rpc", "GET", reader);
    EXPECT_NE(span3, nullptr) << "NewSpan with method and reader should return a valid Span";

    agent.Shutdown(); // Should not throw

    SUCCEED() << "All NoopAgent methods should execute without throwing exceptions";
}

// NoopTraceContextReader Tests

TEST_F(NoopTest, NoopTraceContextReaderTest) {
    NoopTraceContextReader reader;

    auto value = reader.Get("test-key");
    EXPECT_FALSE(value.has_value()) << "NoopTraceContextReader should always return nullopt";

    auto value2 = reader.Get(HEADER_TRACE_ID);
    EXPECT_FALSE(value2.has_value()) << "NoopTraceContextReader should always return nullopt for any key";
}

} // namespace pinpoint

