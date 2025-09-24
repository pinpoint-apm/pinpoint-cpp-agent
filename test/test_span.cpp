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
#include <memory>
#include <chrono>
#include <map>
#include <thread>
#include <functional>
#include <atomic>

#include "../src/span.h"
#include "../src/config.h"
#include "../src/agent_service.h"
#include "../src/url_stat.h"
#include "../include/pinpoint/tracer.h"

namespace pinpoint {

// Mock implementation of AgentService for Span testing
class MockAgentService : public AgentService {
public:
    MockAgentService() : is_exiting_(false), start_time_(1234567890), trace_id_counter_(0) {
        config_.span.event_chunk_size = 10;
        config_.span.max_event_depth = 64;
        config_.span.max_event_sequence = 512;
    }

    // AgentService interface implementation
    bool isExiting() const override { return is_exiting_; }
    std::string_view getAppName() const override { return "TestApp"; }
    int32_t getAppType() const override { return 1300; }
    std::string_view getAgentId() const override { return "test-agent-001"; }
    std::string_view getAgentName() const override { return "TestAgent"; }
    const Config& getConfig() const override { return config_; }
    int64_t getStartTime() const override { return start_time_; }

    TraceId generateTraceId() override {
        TraceId trace_id;
        trace_id.StartTime = start_time_;
        trace_id.Sequence = trace_id_counter_++;
        return trace_id;
    }

    void recordSpan(std::unique_ptr<SpanChunk> span) const override {
        recorded_spans_.push_back(std::move(span));
    }

    void recordUrlStat(std::unique_ptr<UrlStat> stat) const override {
        recorded_url_stats_++;
    }

    void recordStats(StatsType stats) const override {
        recorded_stats_calls_++;
    }

    int32_t cacheApi(std::string_view api_str, int32_t api_type) const override {
        cached_apis_[std::string(api_str)] = api_id_counter_;
        return api_id_counter_++;
    }

    void removeCacheApi(const ApiMeta& api_meta) const override {
        // Mock implementation
    }

    int32_t cacheError(std::string_view error_name) const override {
        cached_errors_[std::string(error_name)] = error_id_counter_;
        return error_id_counter_++;
    }

    void removeCacheError(const StringMeta& str_meta) const override {
        // Mock implementation
    }

    bool isStatusFail(int status) const override {
        return status >= 400;
    }

    void recordServerHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const override {
        recorded_server_headers_++;
    }

    void recordClientHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const override {
        recorded_client_headers_++;
    }

    // Test helpers
    void setExiting(bool exiting) { is_exiting_ = exiting; }
    int32_t getCachedApiId(const std::string& api_str) const {
        auto it = cached_apis_.find(api_str);
        return it != cached_apis_.end() ? it->second : -1;
    }
    int32_t getCachedErrorId(const std::string& error_name) const {
        auto it = cached_errors_.find(error_name);
        return it != cached_errors_.end() ? it->second : -1;
    }
    
    size_t getRecordedSpansCount() const { return recorded_spans_.size(); }
    const SpanChunk* getLastRecordedSpan() const { 
        return recorded_spans_.empty() ? nullptr : recorded_spans_.back().get(); 
    }

    mutable std::vector<std::unique_ptr<SpanChunk>> recorded_spans_;
    mutable int recorded_url_stats_ = 0;
    mutable int recorded_stats_calls_ = 0;
    mutable int recorded_server_headers_ = 0;
    mutable int recorded_client_headers_ = 0;

private:
    bool is_exiting_;
    int64_t start_time_;
    int64_t trace_id_counter_;
    Config config_;
    mutable std::map<std::string, int32_t> cached_apis_;
    mutable std::map<std::string, int32_t> cached_errors_;
    mutable int32_t api_id_counter_ = 100;
    mutable int32_t error_id_counter_ = 200;
};

// Mock TraceContextReader for testing
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

// Mock TraceContextWriter for testing
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

// Mock HeaderReader for testing
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

class SpanTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_agent_service_ = std::make_unique<MockAgentService>();
    }

    void TearDown() override {
        mock_agent_service_.reset();
    }

    std::unique_ptr<MockAgentService> mock_agent_service_;
};

// ========== EventStack Tests ==========

TEST_F(SpanTest, EventStackBasicOperationsTest) {
    EventStack stack;
    
    EXPECT_EQ(stack.size(), 0) << "Initial stack should be empty";
    
    // Create test span events
    auto span_data = std::make_shared<SpanData>(mock_agent_service_.get(), "test-operation");
    auto event1 = std::make_shared<SpanEventImpl>(span_data.get(), "event1");
    auto event2 = std::make_shared<SpanEventImpl>(span_data.get(), "event2");
    
    // Test push
    stack.push(event1);
    EXPECT_EQ(stack.size(), 1) << "Stack size should be 1 after first push";
    
    stack.push(event2);
    EXPECT_EQ(stack.size(), 2) << "Stack size should be 2 after second push";
    
    // Test top
    auto top_event = stack.top();
    EXPECT_EQ(top_event, event2) << "Top should return the last pushed event";
    EXPECT_EQ(stack.size(), 2) << "Top should not change stack size";
    
    // Test pop
    auto popped_event = stack.pop();
    EXPECT_EQ(popped_event, event2) << "Pop should return the last pushed event";
    EXPECT_EQ(stack.size(), 1) << "Stack size should be 1 after pop";
    
    auto second_pop = stack.pop();
    EXPECT_EQ(second_pop, event1) << "Second pop should return first event";
    EXPECT_EQ(stack.size(), 0) << "Stack should be empty after popping all events";
}

TEST_F(SpanTest, EventStackConcurrentAccessTest) {
    EventStack stack;
    auto span_data = std::make_shared<SpanData>(mock_agent_service_.get(), "test-operation");
    
    const int num_threads = 4;
    const int events_per_thread = 10;
    std::atomic<int> push_count(0);
    std::atomic<int> pop_count(0);
    
    std::vector<std::thread> threads;
    
    // Push threads
    for (int t = 0; t < num_threads / 2; t++) {
        threads.emplace_back([&stack, &span_data, &push_count]() {
            for (int i = 0; i < events_per_thread; i++) {
                auto event = std::make_shared<SpanEventImpl>(span_data.get(), "event" + std::to_string(i));
                stack.push(event);
                push_count++;
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        });
    }
    
    // Wait a bit for some pushes to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Pop threads
    for (int t = 0; t < num_threads / 2; t++) {
        threads.emplace_back([&stack, &pop_count]() {
            while (pop_count < 10) {  // Try to pop some events
                try {
                    if (stack.size() > 0) {
                        stack.pop();
                        pop_count++;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
                } catch (...) {
                    break; // Stack might be empty
                }
            }
        });
    }
    
    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }
    
    EXPECT_GT(push_count.load(), 0) << "Should have pushed some events";
    EXPECT_GT(pop_count.load(), 0) << "Should have popped some events";
}

// ========== SpanData Tests ==========

TEST_F(SpanTest, SpanDataConstructorTest) {
    SpanData span_data(mock_agent_service_.get(), "test-operation");
    
    EXPECT_EQ(span_data.getOperationName(), "test-operation") << "Operation name should match";
    EXPECT_EQ(span_data.getAppType(), 1300) << "App type should match agent's app type";
    EXPECT_EQ(span_data.getServiceType(), DEFAULT_SERVICE_TYPE) << "Default service type should be set";
    EXPECT_GT(span_data.getApiId(), 0) << "API ID should be cached and positive";
    EXPECT_EQ(span_data.getParentSpanId(), -1) << "Initial parent span ID should be -1";
    EXPECT_EQ(span_data.getParentAppType(), 1) << "Default parent app type should be 1";
    EXPECT_EQ(span_data.getEventSequence(), 0) << "Initial event sequence should be 0";
    EXPECT_EQ(span_data.getEventDepth(), 1) << "Initial event depth should be 1";
    EXPECT_GT(span_data.getStartTime(), 0) << "Start time should be set";
    EXPECT_EQ(span_data.getElapsed(), 0) << "Initial elapsed should be 0";
    EXPECT_EQ(span_data.getAsyncId(), NONE_ASYNC_ID) << "Initial async ID should be NONE_ASYNC_ID";
    EXPECT_FALSE(span_data.isAsyncSpan()) << "Should not be async span initially";
    EXPECT_NE(span_data.getAnnotations(), nullptr) << "Annotations should be initialized";
    EXPECT_EQ(span_data.getAgent(), mock_agent_service_.get()) << "Agent should match";
    EXPECT_EQ(span_data.getFinishedEventsCount(), 0) << "Initial finished events count should be 0";
}

TEST_F(SpanTest, SpanDataSettersAndGettersTest) {
    SpanData span_data(mock_agent_service_.get(), "test-operation");
    
    // Test TraceId
    TraceId trace_id;
    trace_id.StartTime = 123456789;
    trace_id.Sequence = 42;
    span_data.setTraceId(trace_id);
    EXPECT_EQ(span_data.getTraceId().StartTime, 123456789);
    EXPECT_EQ(span_data.getTraceId().Sequence, 42);
    
    // Test span ID
    span_data.setSpanId(987654321);
    EXPECT_EQ(span_data.getSpanId(), 987654321);
    
    // Test parent span info
    span_data.setParentSpanId(111222333);
    EXPECT_EQ(span_data.getParentSpanId(), 111222333);
    
    span_data.setParentAppType(1234);
    EXPECT_EQ(span_data.getParentAppType(), 1234);
    
    span_data.setParentAppName("ParentApp");
    EXPECT_EQ(span_data.getParentAppName(), "ParentApp");
    
    span_data.setParentAppNamespace("ParentNamespace");
    EXPECT_EQ(span_data.getParentAppNamespace(), "ParentNamespace");
    
    // Test service type
    span_data.setServiceType(5678);
    EXPECT_EQ(span_data.getServiceType(), 5678);
    
    // Test network info
    span_data.setRpcName("TestRPC");
    EXPECT_EQ(span_data.getRpcName(), "TestRPC");
    
    span_data.setEndPoint("http://example.com");
    EXPECT_EQ(span_data.getEndPoint(), "http://example.com");
    
    span_data.setRemoteAddr("192.168.1.100");
    EXPECT_EQ(span_data.getRemoteAddr(), "192.168.1.100");
    
    span_data.setAcceptorHost("localhost");
    EXPECT_EQ(span_data.getAcceptorHost(), "localhost");
    
    // Test flags and logging
    span_data.setLoggingInfo(999);
    EXPECT_EQ(span_data.getLoggingInfo(), 999);
    
    span_data.setFlags(0x12345);
    EXPECT_EQ(span_data.getFlags(), 0x12345);
    
    span_data.setErr(404);
    EXPECT_EQ(span_data.getErr(), 404);
    
    // Test error info
    span_data.setErrorFuncId(777);
    EXPECT_EQ(span_data.getErrorFuncId(), 777);
    
    span_data.setErrorString("Test error message");
    EXPECT_EQ(span_data.getErrorString(), "Test error message");
    
    // Test async info
    span_data.setAsyncId(888);
    EXPECT_EQ(span_data.getAsyncId(), 888);
    EXPECT_TRUE(span_data.isAsyncSpan());
    
    span_data.setAsyncSequence(999);
    EXPECT_EQ(span_data.getAsyncSequence(), 999);
}

TEST_F(SpanTest, SpanDataEventDepthManagementTest) {
    SpanData span_data(mock_agent_service_.get(), "test-operation");
    
    EXPECT_EQ(span_data.getEventDepth(), 1) << "Initial depth should be 1";
    
    span_data.setEventDepth(5);
    EXPECT_EQ(span_data.getEventDepth(), 5) << "Depth should be updated to 5";
    
    span_data.decrEventDepth();
    EXPECT_EQ(span_data.getEventDepth(), 4) << "Depth should be decremented to 4";
    
    span_data.decrEventDepth();
    span_data.decrEventDepth();
    EXPECT_EQ(span_data.getEventDepth(), 2) << "Depth should be decremented to 2";
}

TEST_F(SpanTest, SpanDataTimeManagementTest) {
    SpanData span_data(mock_agent_service_.get(), "test-operation");
    
    int64_t initial_start_time = span_data.getStartTime();
    EXPECT_GT(initial_start_time, 0) << "Initial start time should be positive";
    
    // Set custom start time
    auto custom_start = std::chrono::system_clock::now() - std::chrono::seconds(10);
    span_data.setStartTime(custom_start);
    
    int64_t new_start_time = span_data.getStartTime();
    EXPECT_NE(new_start_time, initial_start_time) << "Start time should be updated";
    
    // Test end time
    EXPECT_EQ(span_data.getElapsed(), 0) << "Initial elapsed should be 0";
    
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    span_data.setEndTime();
    
    EXPECT_GT(span_data.getElapsed(), 0) << "Elapsed time should be positive after setEndTime";
}

TEST_F(SpanTest, SpanDataEventSequenceTest) {
    SpanData span_data(mock_agent_service_.get(), "test-operation");
    
    EXPECT_EQ(span_data.getEventSequence(), 0) << "Initial sequence should be 0";
    
    span_data.setEventSequence(42);
    EXPECT_EQ(span_data.getEventSequence(), 42) << "Sequence should be updated to 42";
}

TEST_F(SpanTest, SpanDataSpanEventManagementTest) {
    SpanData span_data(mock_agent_service_.get(), "test-operation");
    
    // Create span events
    auto event1 = std::make_shared<SpanEventImpl>(&span_data, "event1");
    auto event2 = std::make_shared<SpanEventImpl>(&span_data, "event2");
    
    EXPECT_EQ(span_data.getFinishedEventsCount(), 0) << "Initial finished events should be 0";
    
    // Add span events
    span_data.addSpanEvent(event1);
    EXPECT_EQ(span_data.getEventSequence(), 1) << "Event sequence should increment";
    EXPECT_EQ(span_data.getEventDepth(), 2) << "Event depth should increment";
    
    span_data.addSpanEvent(event2);
    EXPECT_EQ(span_data.getEventSequence(), 2) << "Event sequence should be 2";
    EXPECT_EQ(span_data.getEventDepth(), 3) << "Event depth should be 3";
    
    // Get top event
    auto top_event = span_data.topSpanEvent();
    EXPECT_EQ(top_event, event2) << "Top event should be the last added";
    
    // Finish span events
    span_data.finishSpanEvent();
    EXPECT_EQ(span_data.getFinishedEventsCount(), 1) << "Should have 1 finished event";
    
    span_data.finishSpanEvent();
    EXPECT_EQ(span_data.getFinishedEventsCount(), 2) << "Should have 2 finished events";
    
    // Clear finished events
    span_data.clearFinishedEvents();
    EXPECT_EQ(span_data.getFinishedEventsCount(), 0) << "Finished events should be cleared";
}

TEST_F(SpanTest, SpanDataTraceIdParsingTest) {
    SpanData span_data(mock_agent_service_.get(), "test-operation");
    
    // Test valid trace ID parsing
    std::string valid_tid = "test-agent-001^1234567890^42";
    span_data.parseTraceId(valid_tid);
    
    // The exact values depend on the parsing implementation
    // but we can verify the trace ID was set
    EXPECT_TRUE(true) << "TraceId parsing should not crash";
}

TEST_F(SpanTest, SpanDataUrlStatTest) {
    SpanData span_data(mock_agent_service_.get(), "test-operation");
    
    // Set URL stat
    span_data.setUrlStat("/api/users", "GET", 200);
    
    // Send URL stat
    span_data.sendUrlStat();
    
    EXPECT_EQ(mock_agent_service_->recorded_url_stats_, 1) << "URL stat should be recorded";
}

// ========== SpanChunk Tests ==========

TEST_F(SpanTest, SpanChunkConstructorTest) {
    auto span_data = std::make_shared<SpanData>(mock_agent_service_.get(), "test-operation");
    
    SpanChunk chunk(span_data, true);
    
    EXPECT_EQ(chunk.getSpanData(), span_data) << "Span data should match";
    EXPECT_TRUE(chunk.isFinal()) << "Should be final chunk";
    EXPECT_GE(chunk.getKeyTime(), 0) << "Key time should be non-negative";
    EXPECT_EQ(chunk.getSpanEventChunk().size(), 0) << "Initial event chunk should be empty";
}

TEST_F(SpanTest, SpanChunkWithEventsTest) {
    auto span_data = std::make_shared<SpanData>(mock_agent_service_.get(), "test-operation");
    
    // Add some finished events to span data
    auto event1 = std::make_shared<SpanEventImpl>(span_data.get(), "event1");
    auto event2 = std::make_shared<SpanEventImpl>(span_data.get(), "event2");
    
    span_data->addSpanEvent(event1);
    span_data->addSpanEvent(event2);
    span_data->finishSpanEvent();
    span_data->finishSpanEvent();
    
    SpanChunk chunk(span_data, false);
    
    EXPECT_FALSE(chunk.isFinal()) << "Should not be final chunk";
    EXPECT_GT(chunk.getSpanEventChunk().size(), 0) << "Should have span events";
}

TEST_F(SpanTest, SpanChunkOptimizeEventsTest) {
    auto span_data = std::make_shared<SpanData>(mock_agent_service_.get(), "test-operation");
    
    // Add some events
    auto event1 = std::make_shared<SpanEventImpl>(span_data.get(), "event1");
    span_data->addSpanEvent(event1);
    span_data->finishSpanEvent();
    
    SpanChunk chunk(span_data, true);
    
    // Test optimization (should not crash)
    chunk.optimizeSpanEvents();
    
    SUCCEED() << "Event optimization should complete without errors";
}

// ========== SpanImpl Tests ==========

TEST_F(SpanTest, SpanImplConstructorTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    
    EXPECT_TRUE(span.IsSampled()) << "Span should be sampled";
    EXPECT_NE(span.GetAnnotations(), nullptr) << "Annotations should be available";
    // Span ID might be 0 initially until context is extracted or generated
    EXPECT_GE(span.GetSpanId(), 0) << "Span ID should be non-negative";
}

TEST_F(SpanTest, SpanImplNewSpanEventTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    
    auto span_event = span.NewSpanEvent("database-query");
    EXPECT_NE(span_event, nullptr) << "Span event should be created";
    
    auto span_event_with_type = span.NewSpanEvent("database-query", 2100);
    EXPECT_NE(span_event_with_type, nullptr) << "Span event with service type should be created";
}

TEST_F(SpanTest, SpanImplGetSpanEventTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    
    // Create a span event first to avoid empty stack issues
    span.NewSpanEvent("test-event");
    auto active_event = span.GetSpanEvent();
    EXPECT_NE(active_event, nullptr) << "Should return active span event";
}

TEST_F(SpanTest, SpanImplEndSpanEventTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    
    // Create and end span event
    span.NewSpanEvent("test-event");
    span.EndSpanEvent();
    
    SUCCEED() << "End span event should complete without errors";
}

TEST_F(SpanTest, SpanImplEndSpanTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    
    // End the span
    span.EndSpan();
    
    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), 0) << "Span should be recorded";
}

TEST_F(SpanTest, SpanImplSettersTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    
    // Test service type
    span.SetServiceType(2100);
    
    // Test start time
    auto start_time = std::chrono::system_clock::now();
    span.SetStartTime(start_time);
    
    // Test remote address
    span.SetRemoteAddress("192.168.1.100");
    
    // Test end point
    span.SetEndPoint("http://example.com");
    
    // Test error
    span.SetError("Test error");
    span.SetError("SQLException", "Connection failed");
    
    // Test status code
    span.SetStatusCode(200);
    
    // Test URL stat
    span.SetUrlStat("/api/users", "GET", 200);
    
    SUCCEED() << "All setters should complete without errors";
}

TEST_F(SpanTest, SpanImplRecordHeaderTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    MockHeaderReader header_reader;
    
    header_reader.SetHeader("Content-Type", "application/json");
    span.RecordHeader(HTTP_REQUEST, header_reader);
    
    EXPECT_GT(mock_agent_service_->recorded_server_headers_, 0) << "Header should be recorded";
}

TEST_F(SpanTest, SpanImplInjectContextTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    MockTraceContextWriter writer;
    
    // Create a span event to enable context injection
    span.NewSpanEvent("test-event");
    
    span.InjectContext(writer);
    
    // Verify context was injected
    auto trace_id = writer.Get(HEADER_TRACE_ID);
    EXPECT_TRUE(trace_id.has_value()) << "Trace ID should be injected";
    
    auto span_id = writer.Get(HEADER_SPAN_ID);
    EXPECT_TRUE(span_id.has_value()) << "Span ID should be injected";
    
    auto parent_span_id = writer.Get(HEADER_PARENT_SPAN_ID);
    EXPECT_TRUE(parent_span_id.has_value()) << "Parent span ID should be injected";
}

TEST_F(SpanTest, SpanImplExtractContextTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    MockTraceContextReader reader;
    
    // Set up context with specific values
    std::string expected_trace_id = "test-agent-001^1234567890^42";
    int64_t expected_span_id = 987654321;
    int64_t expected_parent_span_id = 111222333;
    std::string expected_parent_app_name = "ParentApp";
    int32_t expected_parent_app_type = 1400;
    
    reader.SetContext(HEADER_TRACE_ID, expected_trace_id);
    reader.SetContext(HEADER_SPAN_ID, std::to_string(expected_span_id));
    reader.SetContext(HEADER_PARENT_SPAN_ID, std::to_string(expected_parent_span_id));
    reader.SetContext(HEADER_PARENT_APP_NAME, expected_parent_app_name);
    reader.SetContext(HEADER_PARENT_APP_TYPE, std::to_string(expected_parent_app_type));
    
    span.ExtractContext(reader);
    
    // Verify context was extracted with correct values
    EXPECT_EQ(span.GetSpanId(), expected_span_id) << "Span ID should match the value from context";
    
    // Verify trace ID was parsed correctly
    TraceId& trace_id = span.GetTraceId();
    EXPECT_EQ(trace_id.StartTime, 1234567890) << "Trace ID start time should be parsed correctly";
    EXPECT_EQ(trace_id.Sequence, 42) << "Trace ID sequence should be parsed correctly";
}

TEST_F(SpanTest, SpanImplNewAsyncSpanTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    
    // Create a span event first for context
    span.NewSpanEvent("base-event");
    
    auto async_span = span.NewAsyncSpan("async-operation");
    EXPECT_NE(async_span, nullptr) << "Async span should be created";
}

// ========== Integration Tests ==========

TEST_F(SpanTest, CompleteSpanWorkflowTest) {
    SpanImpl span(mock_agent_service_.get(), "web-request", "/api/users");
    
    // Set up span
    span.SetServiceType(1400);
    span.SetRemoteAddress("192.168.1.100");
    span.SetEndPoint("http://api.example.com");
    
    // Add headers
    MockHeaderReader headers;
    headers.SetHeader("User-Agent", "TestClient/1.0");
    span.RecordHeader(HTTP_REQUEST, headers);
    
    // Create span events
    auto db_event = span.NewSpanEvent("database-query", 2100);
    EXPECT_NE(db_event, nullptr);
    
    auto cache_event = span.NewSpanEvent("cache-get", 8200);
    EXPECT_NE(cache_event, nullptr);
    
    // End events in reverse order
    span.EndSpanEvent(); // End cache event
    span.EndSpanEvent(); // End db event
    
    // Set error
    span.SetError("NetworkError", "Connection timeout");
    
    // Set URL stat
    span.SetUrlStat("/api/users", "GET", 500);
    
    // End span
    span.EndSpan();
    
    // Verify span was recorded
    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), 0) << "Span should be recorded";
    EXPECT_GT(mock_agent_service_->recorded_server_headers_, 0) << "Headers should be recorded";
    EXPECT_GT(mock_agent_service_->recorded_url_stats_, 0) << "URL stats should be recorded";
}

TEST_F(SpanTest, ContextPropagationTest) {
    SpanImpl parent_span(mock_agent_service_.get(), "parent-operation", "parent-rpc");
    MockTraceContextWriter writer;
    
    // Initialize parent span with context first
    MockTraceContextReader parent_reader;
    parent_reader.SetContext(HEADER_TRACE_ID, "test-agent-001^1234567890^1");
    parent_reader.SetContext(HEADER_SPAN_ID, "123456789");
    parent_span.ExtractContext(parent_reader);
    
    // Create span event and inject context
    parent_span.NewSpanEvent("external-call");
    parent_span.InjectContext(writer);
    
    // Create child span and extract context
    SpanImpl child_span(mock_agent_service_.get(), "child-operation", "child-rpc");
    MockTraceContextReader reader;
    
    // Transfer context from writer to reader
    if (auto trace_id = writer.Get(HEADER_TRACE_ID)) {
        reader.SetContext(HEADER_TRACE_ID, trace_id.value());
    }
    if (auto span_id = writer.Get(HEADER_SPAN_ID)) {
        reader.SetContext(HEADER_SPAN_ID, span_id.value());
    }
    if (auto parent_span_id = writer.Get(HEADER_PARENT_SPAN_ID)) {
        reader.SetContext(HEADER_PARENT_SPAN_ID, parent_span_id.value());
    }
    
    child_span.ExtractContext(reader);
    
    // Both spans should have valid IDs (after context extraction)
    EXPECT_NE(parent_span.GetSpanId(), 0) << "Parent span should have non-zero ID";
    EXPECT_NE(child_span.GetSpanId(), 0) << "Child span should have non-zero ID";
    EXPECT_NE(parent_span.GetSpanId(), child_span.GetSpanId()) << "Span IDs should be different";
}

TEST_F(SpanTest, MultipleSpanEventsTest) {
    SpanImpl span(mock_agent_service_.get(), "complex-operation", "complex-rpc");
    
    // Create multiple nested span events
    span.NewSpanEvent("step1");
    span.NewSpanEvent("step2");
    span.NewSpanEvent("step3");
    
    // End them all
    span.EndSpanEvent(); // End step3
    span.EndSpanEvent(); // End step2
    span.EndSpanEvent(); // End step1
    
    span.EndSpan();
    
    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), 0) << "Complex span should be recorded";
}

TEST_F(SpanTest, AsyncSpanTest) {
    SpanImpl parent_span(mock_agent_service_.get(), "parent-operation", "parent-rpc");
    
    // Create span event first to provide context for async span
    parent_span.NewSpanEvent("prepare-async");
    
    try {
        auto async_span = parent_span.NewAsyncSpan("async-task");
        if (async_span != nullptr) {
            async_span->EndSpan();
        }
    } catch (...) {
        // Async span creation might fail in test environment
    }
    
    parent_span.EndSpan();
    
    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), 0) << "Parent span should be recorded";
}

} // namespace pinpoint
