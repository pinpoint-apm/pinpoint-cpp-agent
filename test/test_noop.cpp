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
#include "../src/stat.h"
#include "../include/pinpoint/tracer.h"
#include "mock_agent_service.h"
#include "mock_helpers.h"

namespace pinpoint {

class NoopTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_agent_service_ = std::make_unique<MockAgentService>();
        mock_agent_service_->setExiting(false);
        mock_agent_service_->setAppName("mock-app");
        mock_agent_service_->setAgentId("mock-agent");
        mock_agent_service_->setAgentName("mock-agent-name");
        mock_agent_service_->setStartTime(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        auto& cfg = mock_agent_service_->mutableConfig();
        cfg->span.event_chunk_size = 10;
        cfg->span.max_event_depth = 32;
        cfg->span.queue_size = 1024;
        cfg->http.url_stat.enable = true;
        cfg->http.url_stat.limit = 1024;
        cfg->http.url_stat.trim_path_depth = 3;
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
    span_event.SetSqlQuery("SELECT * FROM users WHERE id = ?", "1");

    MockHeaderReader reader;
    span_event.RecordHeader(HTTP_REQUEST, reader);

    SUCCEED() << "All NoopSpanEvent methods should execute without throwing exceptions";
}

TEST_F(NoopTest, NoopSpanEventSetErrorWithCallStackTest) {
    NoopSpanEvent span_event;

    MockCallStackReader stack_reader;
    stack_reader.AddFrame("test-module", "test_function", "test.cpp", 42);
    span_event.SetError("TestError", "Test error message", stack_reader);

    SUCCEED() << "SetError with CallStackReader should execute without throwing";
}

// NoopAnnotation Tests

TEST_F(NoopTest, NoopAnnotationAllMethodsTest) {
    NoopAnnotation annotation;

    // All these should be no-ops and not throw exceptions
    annotation.AppendInt(1, 42);
    annotation.AppendLong(1, 123456789LL);
    annotation.AppendString(2, "test-string");
    annotation.AppendStringString(3, "key", "value");
    annotation.AppendIntStringString(4, 100, "key", "value");
    annotation.AppendBytesStringString(5, {0x01, 0x02, 0x03}, "key", "value");
    annotation.AppendLongIntIntByteByteString(6, 123456789L, 10, 20, 30, 40, "test");

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

// NoopSpan additional tests

TEST_F(NoopTest, NoopSpanSetLoggingAndRecordHeaderTest) {
    NoopSpan span;

    MockTraceContextWriter writer;
    span.SetLogging(writer);

    MockHeaderReader header_reader;
    header_reader.SetHeader("Content-Type", "application/json");
    span.RecordHeader(HTTP_REQUEST, header_reader);

    SUCCEED() << "SetLogging and RecordHeader should execute without throwing";
}

TEST_F(NoopTest, NoopSpanNewAsyncSpanReturnsNoopTest) {
    NoopSpan span;

    auto async_span = span.NewAsyncSpan("async-op");
    ASSERT_NE(async_span, nullptr);
    EXPECT_FALSE(async_span->IsSampled()) << "Async span from NoopSpan should not be sampled";
    EXPECT_EQ(async_span->GetSpanId(), 0) << "Async span from NoopSpan should have span ID 0";
}

TEST_F(NoopTest, NoopSpanEmptyTraceIdTest) {
    NoopSpan span;

    auto& trace_id = span.GetTraceId();
    EXPECT_TRUE(trace_id.AgentId.empty()) << "NoopSpan TraceId AgentId should be empty";
    EXPECT_EQ(trace_id.StartTime, 0) << "NoopSpan TraceId StartTime should be 0";
    EXPECT_EQ(trace_id.Sequence, 0) << "NoopSpan TraceId Sequence should be 0";
}

// NoopAgent additional tests

TEST_F(NoopTest, NoopAgentReturnedSpansAreNoopTest) {
    NoopAgent agent;

    auto span1 = agent.NewSpan("op", "rpc");
    ASSERT_NE(span1, nullptr);
    EXPECT_FALSE(span1->IsSampled());
    EXPECT_EQ(span1->GetSpanId(), 0);

    MockTraceContextReader reader;
    auto span2 = agent.NewSpan("op", "rpc", reader);
    ASSERT_NE(span2, nullptr);
    EXPECT_FALSE(span2->IsSampled());

    auto span3 = agent.NewSpan("op", "rpc", "GET", reader);
    ASSERT_NE(span3, nullptr);
    EXPECT_FALSE(span3->IsSampled());
}

// Noop holder class tests

TEST_F(NoopTest, NoopHolderReturnsSameInstancesTest) {
    Noop noop;

    auto agent1 = noop.agent();
    auto agent2 = noop.agent();
    EXPECT_EQ(agent1.get(), agent2.get()) << "Noop holder should return same agent instance";

    auto span1 = noop.span();
    auto span2 = noop.span();
    EXPECT_EQ(span1.get(), span2.get()) << "Noop holder should return same span instance";

    auto event1 = noop.spanEvent();
    auto event2 = noop.spanEvent();
    EXPECT_EQ(event1.get(), event2.get()) << "Noop holder should return same spanEvent instance";

    auto anno1 = noop.annotation();
    auto anno2 = noop.annotation();
    EXPECT_EQ(anno1.get(), anno2.get()) << "Noop holder should return same annotation instance";
}

TEST_F(NoopTest, NoopHolderAllInstancesAreValidTest) {
    Noop noop;

    EXPECT_NE(noop.agent(), nullptr);
    EXPECT_NE(noop.span(), nullptr);
    EXPECT_NE(noop.spanEvent(), nullptr);
    EXPECT_NE(noop.annotation(), nullptr);

    EXPECT_FALSE(noop.agent()->Enable());
    EXPECT_FALSE(noop.span()->IsSampled());
}

// UnsampledSpan additional tests

TEST_F(NoopTest, UnsampledSpanDoubleEndSpanTest) {
    UnsampledSpan span(mock_agent_service_.get());

    span.SetUrlStat("/api/test", "GET", 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    span.EndSpan();
    EXPECT_EQ(mock_agent_service_->recorded_url_stats_, 1);

    // Second EndSpan should be safe (url_stat_ already moved)
    span.EndSpan();
    EXPECT_EQ(mock_agent_service_->recorded_url_stats_, 1) << "Double EndSpan should not record URL stat twice";
}

TEST_F(NoopTest, UnsampledSpanWithFailStatusUrlStatTest) {
    UnsampledSpan span(mock_agent_service_.get());

    span.SetUrlStat("/api/error", "POST", 500);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    span.EndSpan();

    EXPECT_EQ(mock_agent_service_->recorded_url_stats_, 1);
    EXPECT_EQ(mock_agent_service_->last_url_stat_status_code_, 500);
}

TEST_F(NoopTest, UnsampledSpanInjectContextDoesNotSetOtherHeadersTest) {
    UnsampledSpan span(mock_agent_service_.get());
    MockTraceContextWriter writer;

    span.InjectContext(writer);

    EXPECT_TRUE(writer.Get(HEADER_SAMPLED).has_value()) << "Sampled header should be set";
    EXPECT_FALSE(writer.Get(HEADER_TRACE_ID).has_value()) << "TraceId header should NOT be set for unsampled";
    EXPECT_FALSE(writer.Get(HEADER_SPAN_ID).has_value()) << "SpanId header should NOT be set for unsampled";
    EXPECT_FALSE(writer.Get(HEADER_PARENT_SPAN_ID).has_value()) << "ParentSpanId header should NOT be set for unsampled";
}

// NoopTraceContextReader Tests

TEST_F(NoopTest, NoopTraceContextReaderTest) {
    NoopTraceContextReader reader;

    auto value = reader.Get("test-key");
    EXPECT_FALSE(value.has_value()) << "NoopTraceContextReader should always return nullopt";

    auto value2 = reader.Get(HEADER_TRACE_ID);
    EXPECT_FALSE(value2.has_value()) << "NoopTraceContextReader should always return nullopt for any key";
}

TEST_F(NoopTest, NoopTraceContextReaderAllHeaderKeysTest) {
    NoopTraceContextReader reader;

    EXPECT_FALSE(reader.Get(HEADER_SAMPLED).has_value());
    EXPECT_FALSE(reader.Get(HEADER_SPAN_ID).has_value());
    EXPECT_FALSE(reader.Get(HEADER_PARENT_SPAN_ID).has_value());
    EXPECT_FALSE(reader.Get(HEADER_PARENT_APP_NAME).has_value());
    EXPECT_FALSE(reader.Get(HEADER_PARENT_APP_TYPE).has_value());
    EXPECT_FALSE(reader.Get(HEADER_HOST).has_value());
}

} // namespace pinpoint

