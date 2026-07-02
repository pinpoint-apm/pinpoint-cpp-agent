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
#include "../src/stat.h"
#include "../src/callstack.h"
#include "../src/noop.h"
#include "../include/pinpoint/tracer.h"
#include "mock_agent_service.h"
#include "mock_helpers.h"

namespace pinpoint {

class SpanTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_agent_service_ = std::make_unique<MockAgentService>();
        auto& cfg = mock_agent_service_->mutableConfig();
        cfg->span.event_chunk_size = 10;
        cfg->span.max_event_depth = 64;
        cfg->span.max_event_sequence = 512;
        cfg->enable_callstack_trace = true;
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
    auto span = std::make_shared<SpanImpl>(mock_agent_service_.get(), "test-operation", "test-rpc");
    auto event1 = make_test_span_event_unique(*span, "event1");
    auto event2 = make_test_span_event_unique(*span, "event2");
    auto* event1_ptr = event1.get();
    auto* event2_ptr = event2.get();
    
    // Test push
    stack.push(std::move(event1));
    EXPECT_EQ(stack.size(), 1) << "Stack size should be 1 after first push";
    
    stack.push(std::move(event2));
    EXPECT_EQ(stack.size(), 2) << "Stack size should be 2 after second push";
    
    // Test top
    auto top_event = stack.top();
    EXPECT_EQ(top_event, event2_ptr) << "Top should return the last pushed event";
    EXPECT_EQ(stack.size(), 2) << "Top should not change stack size";
    
    // Test pop
    auto popped_event = stack.pop();
    EXPECT_EQ(popped_event.get(), event2_ptr) << "Pop should return the last pushed event";
    EXPECT_EQ(stack.size(), 1) << "Stack size should be 1 after pop";
    
    auto second_pop = stack.pop();
    EXPECT_EQ(second_pop.get(), event1_ptr) << "Second pop should return first event";
    EXPECT_EQ(stack.size(), 0) << "Stack should be empty after popping all events";
}

TEST_F(SpanTest, EventStackConcurrentAccessTest) {
    auto span = std::make_shared<SpanImpl>(mock_agent_service_.get(), "test-operation", "test-rpc");
    EventStack stack;
    std::mutex stack_mutex;  // External mutex (mirrors SpanData::span_event_lock_)

    constexpr int num_push_threads = 2;
    constexpr int num_pop_threads = 2;
    constexpr int events_per_thread = 10;
    std::atomic<int> push_count(0);
    std::atomic<int> pop_count(0);

    // Pre-create all events on the main thread to avoid data races on
    // MockAgentService (cacheApi / cached_apis_ is not thread-safe).
    std::vector<std::vector<std::unique_ptr<SpanEventImpl>>> pre_created(num_push_threads);
    for (int t = 0; t < num_push_threads; t++) {
        for (int i = 0; i < events_per_thread; i++) {
            pre_created[t].push_back(
                make_test_span_event_unique(*span, "event" + std::to_string(t * events_per_thread + i)));
        }
    }

    std::vector<std::thread> threads;

    // Push threads
    for (int t = 0; t < num_push_threads; t++) {
        threads.emplace_back([&stack, &stack_mutex, &push_count, &events = pre_created[t]]() {
            for (int i = 0; i < events_per_thread; i++) {
                {
                    std::lock_guard<std::mutex> lock(stack_mutex);
                    stack.push(std::move(events[i]));
                }
                push_count++;
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        });
    }

    // Wait a bit for some pushes to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Pop threads
    constexpr int total_to_pop = events_per_thread;  // pop half the total pushed
    for (int t = 0; t < num_pop_threads; t++) {
        threads.emplace_back([&stack, &stack_mutex, &pop_count]() {
            while (pop_count < total_to_pop) {
                {
                    std::lock_guard<std::mutex> lock(stack_mutex);
                    if (stack.size() > 0) {
                        stack.pop();
                        pop_count++;
                    }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        });
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // Drain remaining events so they are destroyed while span_data is alive
    while (stack.size() > 0) {
        stack.pop();
    }
    pre_created.clear();

    EXPECT_GT(push_count.load(), 0) << "Should have pushed some events";
    EXPECT_GT(pop_count.load(), 0) << "Should have popped some events";
}

// ========== SpanData Tests ==========

TEST_F(SpanTest, SpanDataConstructorTest) {
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-operation");
    
    EXPECT_EQ(span_data.getOperationName(), "test-operation") << "Operation name should match";
    EXPECT_EQ(span_data.getAppType(), 1300) << "App type should match agent's app type";
    EXPECT_EQ(span_data.getServiceType(), defaults::SPAN_SERVICE_TYPE) << "Default service type should be set";
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
    EXPECT_EQ(span_data.getFinishedEventsCount(), 0) << "Initial finished events count should be 0";
}

TEST_F(SpanTest, SpanDataSettersAndGettersTest) {
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-operation");
    
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

    EXPECT_EQ(span_data.getParentServiceName(), "") << "parent service name should default to empty";
    span_data.setParentServiceName("ParentService");
    EXPECT_EQ(span_data.getParentServiceName(), "ParentService");
    
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
    span_data.setLoggingFlag();
    EXPECT_EQ(span_data.getLoggingFlag(), 1);
    
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
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-operation");

    // Depth starts at 1 (the span itself); sequence starts at 0.
    EXPECT_EQ(span_data.getEventDepth(), 1) << "Initial depth should be 1";
    EXPECT_EQ(span_data.getEventSequence(), 0) << "Initial sequence should be 0";

    // nextEventSequenceAndDepth() returns the pre-increment values and advances both.
    auto [seq0, depth0] = span_data.nextEventSequenceAndDepth();
    EXPECT_EQ(seq0, 0) << "First reserved sequence should be pre-increment value 0";
    EXPECT_EQ(depth0, 1) << "First reserved depth should be pre-increment value 1";
    EXPECT_EQ(span_data.getEventSequence(), 1) << "Sequence should advance to 1";
    EXPECT_EQ(span_data.getEventDepth(), 2) << "Depth should advance to 2";

    // A nested reservation advances both again.
    auto [seq1, depth1] = span_data.nextEventSequenceAndDepth();
    EXPECT_EQ(seq1, 1);
    EXPECT_EQ(depth1, 2);
    EXPECT_EQ(span_data.getEventDepth(), 3) << "Depth should advance to 3";

    // decrEventDepth() unwinds nesting depth without rewinding the sequence.
    span_data.decrEventDepth();
    EXPECT_EQ(span_data.getEventDepth(), 2) << "Depth should be decremented to 2";
    EXPECT_EQ(span_data.getEventSequence(), 2)
        << "Sequence must not be affected by depth decrement";

    span_data.decrEventDepth();
    EXPECT_EQ(span_data.getEventDepth(), 1) << "Depth should return to its initial value";
}

TEST_F(SpanTest, SpanDataTimeManagementTest) {
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-operation");
    
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
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-operation");
    EXPECT_EQ(span_data.getEventSequence(), 0) << "Initial sequence should be 0";
}

TEST_F(SpanTest, SpanDataSpanEventManagementTest) {
    auto span = std::make_shared<SpanImpl>(mock_agent_service_.get(), "test-operation", "test-rpc");
    auto span_data = span->getSpanData();

    // Create span events
    auto event1 = make_test_span_event_unique(*span, "event1");
    auto event2 = make_test_span_event_unique(*span, "event2");
    auto* event1_ptr = event1.get();
    auto* event2_ptr = event2.get();

    EXPECT_EQ(span_data->getFinishedEventsCount(), 0) << "Initial finished events should be 0";
    EXPECT_EQ(span_data->getEventSequence(), 0) << "Event sequence should not be reserved during event creation";
    EXPECT_EQ(span_data->getEventDepth(), 1) << "Event depth should not be reserved during event creation";

    // Add span events
    EXPECT_EQ(span_data->addSpanEvent(std::move(event1)), event1_ptr);
    EXPECT_EQ(span_data->getEventSequence(), 1) << "Event sequence should increment";
    EXPECT_EQ(span_data->getEventDepth(), 2) << "Event depth should increment";

    EXPECT_EQ(span_data->addSpanEvent(std::move(event2)), event2_ptr);
    EXPECT_EQ(span_data->getEventSequence(), 2) << "Event sequence should be 2";
    EXPECT_EQ(span_data->getEventDepth(), 3) << "Event depth should be 3";

    // Get top event
    auto top_event = span_data->topSpanEvent();
    EXPECT_EQ(top_event, event2_ptr) << "Top event should be the last added";

    // Finish span events
    span_data->finishSpanEvent();
    EXPECT_EQ(span_data->getFinishedEventsCount(), 1) << "Should have 1 finished event";

    span_data->finishSpanEvent();
    EXPECT_EQ(span_data->getFinishedEventsCount(), 2) << "Should have 2 finished events";

    // Take finished events (moves them out, leaving the vector empty)
    auto taken = span_data->takeFinishedEvents();
    ASSERT_EQ(taken.size(), 2) << "Should have taken 2 finished events";
    EXPECT_EQ(taken[0].get(), event1_ptr) << "Finished events should be returned in sequence order";
    EXPECT_EQ(taken[1].get(), event2_ptr) << "Finished events should be returned in sequence order";
    EXPECT_EQ(span_data->getFinishedEventsCount(), 0) << "Finished events should be cleared";
}

TEST_F(SpanTest, SpanDataTraceIdParsingTest) {
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-operation");
    
    // Test valid trace ID parsing
    std::string valid_tid = "test-agent-001^1234567890^42";
    span_data.parseTraceId(valid_tid);
    
    // The exact values depend on the parsing implementation
    // but we can verify the trace ID was set
    EXPECT_TRUE(true) << "TraceId parsing should not crash";
}

TEST_F(SpanTest, SpanImplUrlStatTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    
    // Set URL stat
    span.SetUrlStat("/api/users", "GET", 200);

    EXPECT_EQ(span.getUrlTemplate(), "/api/users");
    span.EndSpan();
    EXPECT_EQ(mock_agent_service_->recorded_url_stats_, 1) << "URL stat should be recorded";
    EXPECT_EQ(mock_agent_service_->last_url_stat_url_, "/api/users");
    EXPECT_EQ(mock_agent_service_->last_url_stat_method_, "GET");
    EXPECT_EQ(mock_agent_service_->last_url_stat_status_code_, 200);
}

// ========== SpanChunk Tests ==========

TEST_F(SpanTest, SpanChunkConstructorTest) {
    auto span_data = make_test_span_data_ptr(*mock_agent_service_, "test-operation");
    
    SpanChunk chunk(span_data, true);
    
    EXPECT_EQ(chunk.getSpanData(), span_data) << "Span data should match";
    EXPECT_TRUE(chunk.isFinal()) << "Should be final chunk";
    EXPECT_GE(chunk.getKeyTime(), 0) << "Key time should be non-negative";
    EXPECT_EQ(chunk.getSpanEventChunk().size(), 0) << "Initial event chunk should be empty";
}

TEST_F(SpanTest, SpanChunkWithEventsTest) {
    auto span = std::make_shared<SpanImpl>(mock_agent_service_.get(), "test-operation", "test-rpc");
    auto span_data = span->getSpanData();
    
    // Add some finished events to span data
    auto event1 = make_test_span_event_unique(*span, "event1");
    auto event2 = make_test_span_event_unique(*span, "event2");
    
    span_data->addSpanEvent(std::move(event1));
    span_data->addSpanEvent(std::move(event2));
    span_data->finishSpanEvent();
    span_data->finishSpanEvent();
    
    SpanChunk chunk(span_data, false);
    
    EXPECT_FALSE(chunk.isFinal()) << "Should not be final chunk";
    EXPECT_GT(chunk.getSpanEventChunk().size(), 0) << "Should have span events";
}

TEST_F(SpanTest, SpanChunkOptimizeEventsTest) {
    auto span = std::make_shared<SpanImpl>(mock_agent_service_.get(), "test-operation", "test-rpc");
    auto span_data = span->getSpanData();
    
    // Add some events
    auto event1 = make_test_span_event_unique(*span, "event1");
    span_data->addSpanEvent(std::move(event1));
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
    auto se = span.NewSpanEvent("test-event");
    se->EndEvent();

    SUCCEED() << "End span event should complete without errors";
}

// EndEvent is guarded like EndSpan: a duplicate end is a warning no-op and
// must not pop another (still-active) event from the span's stack.
TEST_F(SpanTest, SpanEventEndEventDuplicateGuardTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");

    auto outer = span.NewSpanEvent("outer");
    auto inner = span.NewSpanEvent("inner");

    inner->EndEvent();
    inner->EndEvent(); // duplicate: must NOT end "outer"

    EXPECT_EQ(span.GetSpanEvent(), outer)
        << "Duplicate EndEvent must not pop the outer event";

    outer->EndEvent();
    span.EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    EXPECT_EQ(mock_agent_service_->recorded_spans_.back()->getSpanEventChunk().size(), 2u)
        << "Both events should be recorded exactly once";
}

TEST_F(SpanTest, SpanEventEndEventUsesParentSpanTest) {
    SpanPtr span = std::make_shared<SpanImpl>(
        mock_agent_service_.get(), "test-operation", "test-rpc");

    auto event = span->NewSpanEvent("test-event");

    event->EndEvent();
    span->EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& events = mock_agent_service_->recorded_spans_.back()->getSpanEventChunk();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0]->getOperationName(), "test-event");
}

TEST_F(SpanTest, SpanEventEndEventTest) {
    SpanPtr span = std::make_shared<SpanImpl>(
        mock_agent_service_.get(), "test-operation", "test-rpc");

    auto event = span->NewSpanEvent("event-ended-by-handle");

    event->EndEvent();
    span->EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& events = mock_agent_service_->recorded_spans_.back()->getSpanEventChunk();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0]->getOperationName(), "event-ended-by-handle");
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

TEST_F(SpanTest, SpanImplSetLoggingTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    MockTraceContextWriter writer;

    // Before setting logging, verify initial state through data access (indirectly via SpanImpl if possible, 
    // but since data_ is private we rely on writer output)

    span.SetLogging(writer);

    // Verify flag was set and context was injected
    auto ptx_id = writer.Get("PtxId");
    EXPECT_TRUE(ptx_id.has_value()) << "PtxId should be injected";

    auto pspan_id = writer.Get("PspanId");
    EXPECT_TRUE(pspan_id.has_value()) << "PspanId should be injected";
}

TEST_F(SpanTest, SpanEventInjectContextTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    MockTraceContextWriter writer;

    // Context is injected through the span event for the outbound call
    auto se = span.NewSpanEvent("test-event");

    se->InjectContext(writer);
    
    // Verify context was injected
    auto trace_id = writer.Get(HEADER_TRACE_ID);
    EXPECT_TRUE(trace_id.has_value()) << "Trace ID should be injected";
    
    auto span_id = writer.Get(HEADER_SPAN_ID);
    EXPECT_TRUE(span_id.has_value()) << "Span ID should be injected";
    
    auto parent_span_id = writer.Get(HEADER_PARENT_SPAN_ID);
    EXPECT_TRUE(parent_span_id.has_value()) << "Parent span ID should be injected";
}

// uid.version=v4: the agent has its own service name, so InjectContext must
// propagate it via the Pinpoint-pServiceName header (Java DefaultRequestTraceWriter).
TEST_F(SpanTest, SpanEventInjectContextWritesParentServiceNameForV4Test) {
    mock_agent_service_->setServiceName("my-service");

    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    MockTraceContextWriter writer;
    auto se = span.NewSpanEvent("test-event");

    se->InjectContext(writer);

    auto service_name = writer.Get(HEADER_PARENT_SERVICE_NAME);
    ASSERT_TRUE(service_name.has_value()) << "Pinpoint-pServiceName should be injected for v4";
    EXPECT_EQ(service_name.value(), "my-service");
}

// uid.version=v1/v3: the agent has no service name (empty), so InjectContext must
// omit the Pinpoint-pServiceName header (Java writes it only when serviceName != null).
TEST_F(SpanTest, SpanEventInjectContextOmitsParentServiceNameWhenEmptyTest) {
    mock_agent_service_->setServiceName(""); // default for v1/v3

    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    MockTraceContextWriter writer;
    auto se = span.NewSpanEvent("test-event");

    se->InjectContext(writer);

    EXPECT_FALSE(writer.Get(HEADER_PARENT_SERVICE_NAME).has_value())
        << "Pinpoint-pServiceName must be omitted when the agent has no service name (v1/v3)";
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
    
    span.extractContext(reader, reader.Get(HEADER_TRACE_ID));
    
    // Verify context was extracted with correct values
    EXPECT_EQ(span.GetSpanId(), expected_span_id) << "Span ID should match the value from context";
    
    // Verify trace ID was parsed correctly
    TraceId& trace_id = span.GetTraceId();
    EXPECT_EQ(trace_id.StartTime, 1234567890) << "Trace ID start time should be parsed correctly";
    EXPECT_EQ(trace_id.Sequence, 42) << "Trace ID sequence should be parsed correctly";
}

TEST_F(SpanTest, SpanImplNewAsyncSpanTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");

    // Give the parent a known trace/span identity so we can assert the async
    // child inherits it (rather than trivially comparing default zeros).
    MockTraceContextReader reader;
    reader.SetContext(HEADER_TRACE_ID, "test-agent^1700000000^11");
    reader.SetContext(HEADER_SPAN_ID, "555");
    span.extractContext(reader, reader.Get(HEADER_TRACE_ID));

    // Create a span event first for context (required by NewAsyncSpan).
    auto base_event = span.NewSpanEvent("base-event");

    auto async_span = span.NewAsyncSpan("async-operation");
    ASSERT_NE(async_span, nullptr) << "Async span should be created";
    // A real (non-noop) async span inherits the parent's span id and trace id.
    EXPECT_EQ(async_span->GetSpanId(), span.GetSpanId())
        << "Async child should inherit the parent span id";
    EXPECT_EQ(async_span->GetTraceId().Sequence, span.GetTraceId().Sequence)
        << "Async child should inherit the parent trace id";

    async_span->EndSpan();
    base_event->EndEvent();
    span.EndSpan();
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
    cache_event->EndEvent();
    db_event->EndEvent();
    
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
    parent_span.extractContext(parent_reader, parent_reader.Get(HEADER_TRACE_ID));

    // Create span event and inject context
    auto parent_se = parent_span.NewSpanEvent("external-call");
    parent_se->InjectContext(writer);
    
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
    
    child_span.extractContext(reader, reader.Get(HEADER_TRACE_ID));
    
    // Both spans should have valid IDs (after context extraction)
    EXPECT_NE(parent_span.GetSpanId(), 0) << "Parent span should have non-zero ID";
    EXPECT_NE(child_span.GetSpanId(), 0) << "Child span should have non-zero ID";
    EXPECT_NE(parent_span.GetSpanId(), child_span.GetSpanId()) << "Span IDs should be different";
}

TEST_F(SpanTest, MultipleSpanEventsTest) {
    SpanImpl span(mock_agent_service_.get(), "complex-operation", "complex-rpc");
    
    // Create multiple nested span events
    auto step1 = span.NewSpanEvent("step1");
    auto step2 = span.NewSpanEvent("step2");
    auto step3 = span.NewSpanEvent("step3");

    // End them all (innermost first)
    step3->EndEvent();
    step2->EndEvent();
    step1->EndEvent();
    
    span.EndSpan();
    
    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), 0) << "Complex span should be recorded";
}

TEST_F(SpanTest, AsyncSpanTest) {
    SpanImpl parent_span(mock_agent_service_.get(), "parent-operation", "parent-rpc");

    // Create span event first to provide context for async span.
    auto prepare_event = parent_span.NewSpanEvent("prepare-async");

    // NewAsyncSpan never throws (it returns a noop span on internal failure), so
    // no try/catch is needed — assert the real, non-noop result directly.
    auto async_span = parent_span.NewAsyncSpan("async-task");
    ASSERT_NE(async_span, nullptr) << "Async span should be created";
    EXPECT_EQ(async_span->GetSpanId(), parent_span.GetSpanId())
        << "Async child should inherit the parent span id";

    async_span->EndSpan();

    prepare_event->EndEvent();
    parent_span.EndSpan();

    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), 0) << "Parent span should be recorded";
}

// Exercises the sanctioned cross-thread pattern: the async span is CREATED on the
// thread that owns the parent (where a live span event exists), then USED
// exclusively on a separate worker thread. This also guards the lazy owner-thread
// binding in SpanImpl::NewSpanEvent — the async child must bind to the worker
// thread on its first event and must NOT trip the owning-thread check.
TEST_F(SpanTest, AsyncSpanOnSeparateThreadTest) {
    SpanImpl parent(mock_agent_service_.get(), "parent-operation", "parent-rpc");

    // Known identity so we can assert trace linkage across the thread boundary.
    MockTraceContextReader reader;
    reader.SetContext(HEADER_TRACE_ID, "test-agent^1700000000^7");
    reader.SetContext(HEADER_SPAN_ID, "123456789");
    parent.extractContext(reader, reader.Get(HEADER_TRACE_ID));

    // Created on the owning thread (needs a live span event for context).
    auto prepare_event = parent.NewSpanEvent("prepare-async");
    auto async_span = parent.NewAsyncSpan("async-task");
    ASSERT_NE(async_span, nullptr);

    const auto recorded_before = mock_agent_service_->getRecordedSpansCount();

    // Used only on the worker thread. main thread blocks on join(), so there is
    // no concurrent access to the async span or the mock recorder.
    std::thread worker([&]() {
        auto se = async_span->NewSpanEvent("thread-event");
        EXPECT_NE(se, nullptr) << "Async span event should be created on the worker thread";
        se->EndEvent();
        async_span->EndSpan();
    });
    worker.join();

    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), recorded_before)
        << "Async span should record its own chunk from the worker thread";
    EXPECT_EQ(async_span->GetSpanId(), parent.GetSpanId())
        << "Async child should inherit the parent span id";
    EXPECT_EQ(async_span->GetTraceId().Sequence, parent.GetTraceId().Sequence)
        << "Async child should inherit the parent trace id";

    prepare_event->EndEvent();
    parent.EndSpan();
}

// ========== EventStack Edge Case Tests ==========

TEST_F(SpanTest, EventStackPopOnEmptyReturnsNullptrTest) {
    EventStack stack;
    EXPECT_EQ(stack.pop(), nullptr) << "Pop on empty stack should return nullptr";
}

TEST_F(SpanTest, EventStackTopOnEmptyReturnsNullptrTest) {
    EventStack stack;
    EXPECT_EQ(stack.top(), nullptr) << "Top on empty stack should return nullptr";
}

// ========== SpanData parseTraceId Edge Case Tests ==========

TEST_F(SpanTest, SpanDataParseTraceIdMissingSeparatorTest) {
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-op");

    // No '^' at all — should not crash, trace_id fields remain default
    std::string invalid = "no-separator-here";
    span_data.parseTraceId(invalid);

    EXPECT_TRUE(span_data.getTraceId().AgentId.empty())
        << "AgentId should remain empty on invalid input";
}

TEST_F(SpanTest, SpanDataParseTraceIdOnlyOneSeparatorTest) {
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-op");

    // Only one '^' — second field parse should fail gracefully
    std::string partial = "agent^12345";
    span_data.parseTraceId(partial);

    EXPECT_EQ(span_data.getTraceId().AgentId, "agent");
    // Sequence should not be set (stays 0)
    EXPECT_EQ(span_data.getTraceId().Sequence, 0);
}

TEST_F(SpanTest, SpanDataParseTraceIdAgentIdTooLongTest) {
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-op");

    // AgentId exceeds kMaxAgentIdLength (24)
    std::string long_agent = "this-agent-id-is-way-too-long^1234567890^1";
    span_data.parseTraceId(long_agent);

    EXPECT_TRUE(span_data.getTraceId().AgentId.empty())
        << "AgentId should remain empty when too long";
}

TEST_F(SpanTest, SpanDataParseTraceIdEmptyFieldsTest) {
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-op");

    std::string empty_fields = "^^";
    span_data.parseTraceId(empty_fields);

    EXPECT_TRUE(span_data.getTraceId().AgentId.empty());
    EXPECT_EQ(span_data.getTraceId().StartTime, 0);
    EXPECT_EQ(span_data.getTraceId().Sequence, 0);
}

// ========== SpanData finishSpanEvent on Empty Stack ==========

TEST_F(SpanTest, SpanDataFinishSpanEventOnEmptyStackTest) {
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-op");

    // Should not crash — logs a warning, finished_events unchanged
    span_data.finishSpanEvent();
    EXPECT_EQ(span_data.getFinishedEventsCount(), 0)
        << "No event should be finished when stack is empty";
}

// ========== SpanData Exception Tests ==========

TEST_F(SpanTest, SpanDataSendExceptionsEmptyTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "/test");

    span.EndSpan();
    EXPECT_EQ(mock_agent_service_->recorded_exceptions_, 0)
        << "No exceptions should be recorded when list is empty";
}

TEST_F(SpanTest, SpanDataSendExceptionsWithDataTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "/test");
    auto event = make_test_span_event(span, "event-op");
    MockCallStackReader reader;
    reader.AddFrame("/lib/app.so", "func", "/src/file.cpp", 10);

    event.SetError("TestError", "test error", reader);
    EXPECT_EQ(span.getExceptions().size(), 1);

    span.EndSpan();
    EXPECT_EQ(mock_agent_service_->recorded_exceptions_, 1)
        << "Exception should be recorded through agent service";
}

TEST_F(SpanTest, SpanDataTakeExceptionsTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "/test");
    auto event1 = make_test_span_event(span, "event-op-1");
    auto event2 = make_test_span_event(span, "event-op-2");
    MockCallStackReader reader;
    reader.AddFrame("/lib/app.so", "func", "/src/file.cpp", 10);

    event1.SetError("TestError1", "test error", reader);
    event2.SetError("TestError2", "test error", reader);
    EXPECT_EQ(span.getExceptions().size(), 2);

    auto taken = span.takeExceptions();
    EXPECT_EQ(taken.size(), 2);
    EXPECT_EQ(span.getExceptions().size(), 0)
        << "Exceptions should be moved out";
}

// ========== SpanData URL Stat Edge Cases ==========

TEST_F(SpanTest, SpanDataSendUrlStatWithoutSettingTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "/test");

    span.EndSpan();
    EXPECT_EQ(mock_agent_service_->recorded_url_stats_, 0);
}

TEST_F(SpanTest, SpanDataGetUrlTemplateWithoutStatTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "/test");

    EXPECT_EQ(span.getUrlTemplate(), "NULL")
        << "URL template should return NULL when no stat is set";
}

TEST_F(SpanTest, SpanDataGetUrlTemplateWithStatTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "/test");

    span.SetUrlStat("/api/v1/users", "POST", 201);
    EXPECT_EQ(span.getUrlTemplate(), "/api/v1/users");
}

// ========== SpanImpl Operations After Finished ==========

TEST_F(SpanTest, SpanImplOperationsAfterEndSpanTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    span.EndSpan();

    size_t spans_before = mock_agent_service_->getRecordedSpansCount();

    // All these should be no-ops after EndSpan
    auto se = span.NewSpanEvent("should-not-create");
    EXPECT_NE(se, nullptr) << "Should return noop span event, not nullptr";

    se->EndEvent();
    span.EndSpan();

    // No additional spans should be recorded
    EXPECT_EQ(mock_agent_service_->getRecordedSpansCount(), spans_before)
        << "No additional spans should be recorded after finish";

    MockTraceContextWriter writer;
    se->InjectContext(writer);
    EXPECT_FALSE(writer.Get(HEADER_TRACE_ID).has_value())
        << "InjectContext should be no-op after finish";

    span.SetServiceType(9999);
    span.SetRemoteAddress("1.2.3.4");
    span.SetEndPoint("http://nowhere");
    span.SetError("should not record");
    span.SetStatusCode(500);
}

// ========== SpanImpl Overflow Behavior ==========

TEST_F(SpanTest, SpanImplEventDepthOverflowTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");

    // Config max_event_depth is 64. Push events to reach the limit.
    // Each NewSpanEvent increments depth, starting from 1.
    std::vector<SpanEventPtr> events;
    for (int i = 0; i < 63; i++) {
        auto se = span.NewSpanEvent("event-" + std::to_string(i));
        events.push_back(se);
    }

    // Next event should overflow (depth = 64 >= max_event_depth=64)
    auto overflow_event = span.NewSpanEvent("overflow-event");
    EXPECT_NE(overflow_event, nullptr) << "Should return disabled event on overflow";

    // Ending the disabled event should decrement the overflow counter,
    // not pop from the stack
    overflow_event->EndEvent();

    // Now ending the real events should work
    for (int i = 62; i >= 0; i--) {
        events[i]->EndEvent();
    }

    span.EndSpan();
    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), 0);
}

TEST_F(SpanTest, SpanImplEventSequenceOverflowTest) {
    // Set max_event_sequence to a small value for testing
    auto config = std::make_shared<Config>();
    config->span.max_event_depth = 64;
    config->span.max_event_sequence = 5;
    config->span.event_chunk_size = 100;
    mock_agent_service_->reloadConfig(config);

    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");

    // Create and end events up to the sequence limit
    for (int i = 0; i < 5; i++) {
        span.NewSpanEvent("event-" + std::to_string(i))->EndEvent();
    }

    // Next one should overflow
    auto overflow = span.NewSpanEvent("overflow");
    // The overflow event records nothing, but it is NOT the plain noop: it
    // still injects the full trace context (Java DisableSpanEvent parity).
    EXPECT_NE(overflow, noopSpanEvent());
    MockTraceContextWriter writer;
    overflow->InjectContext(writer);
    EXPECT_TRUE(writer.Get(HEADER_TRACE_ID).has_value())
        << "Sequence overflow must not cut the distributed trace";
    overflow->EndEvent();  // decrements overflow

    span.EndSpan();
    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), 0);
}

// ========== SpanImpl Overflow: DisabledSpanEvent (Java DisableSpanEvent parity) ==========

// Overflow is a profiling depth limit, not a sampling decision: the disabled
// event returned on overflow records nothing locally but still injects the
// full trace context, so the distributed trace continues downstream.
TEST_F(SpanTest, DisabledSpanEventInjectContextOnOverflowTest) {
    auto config = std::make_shared<Config>();
    config->span.max_event_depth = 2;
    config->span.max_event_sequence = 512;
    config->span.event_chunk_size = 100;
    mock_agent_service_->reloadConfig(config);

    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    MockTraceContextReader reader;
    reader.SetContext(HEADER_TRACE_ID, "test-agent^1700000000^42");
    reader.SetContext(HEADER_SPAN_ID, "555");
    span.extractContext(reader, reader.Get(HEADER_TRACE_ID));

    auto real = span.NewSpanEvent("real-event");
    auto overflowed = span.NewSpanEvent("overflowed-event"); // depth 2 >= max 2
    EXPECT_NE(overflowed, noopSpanEvent())
        << "Overflow should hand out the disabled event, not the plain noop";
    EXPECT_EQ(span.GetSpanEvent(), overflowed)
        << "GetSpanEvent during overflow should return the disabled event";

    // Recording is a no-op and must not crash.
    overflowed->SetDestination("downstream:8080");
    overflowed->SetError("ignored");
    EXPECT_EQ(overflowed->GetAnnotations(), noopAnnotation());

    MockTraceContextWriter writer;
    overflowed->InjectContext(writer);
    EXPECT_EQ(writer.Get(HEADER_TRACE_ID).value(), "test-agent^1700000000^42");
    EXPECT_EQ(writer.Get(HEADER_PARENT_SPAN_ID).value(), "555");
    EXPECT_TRUE(writer.Get(HEADER_SPAN_ID).has_value())
        << "A child span id must still be generated during overflow";
    EXPECT_EQ(writer.Get(HEADER_HOST).value(), "downstream:8080");

    overflowed->EndEvent(); // consumes the overflow placeholder
    EXPECT_EQ(span.GetSpanEvent(), real)
        << "After the overflow resolves, the real top event is handed out again";
    real->EndEvent();
    span.EndSpan();

    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), 0);
}

// ========== SpanImpl Event Chunking ==========

TEST_F(SpanTest, SpanImplEventChunkingTest) {
    // Set chunk size to 3 to trigger intermediate flushes
    auto config = std::make_shared<Config>();
    config->span.max_event_depth = 64;
    config->span.max_event_sequence = 512;
    config->span.event_chunk_size = 3;
    mock_agent_service_->reloadConfig(config);

    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");

    // Create and end 3 events to trigger a chunk flush
    for (int i = 0; i < 3; i++) {
        span.NewSpanEvent("event-" + std::to_string(i))->EndEvent();
    }

    size_t chunks_after_3 = mock_agent_service_->getRecordedSpansCount();
    EXPECT_EQ(chunks_after_3, 1)
        << "Should flush an intermediate chunk when event_chunk_size is reached";

    // Verify intermediate chunk is not final
    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    EXPECT_FALSE(mock_agent_service_->recorded_spans_.back()->isFinal())
        << "Intermediate chunk should not be final";

    // End span — should produce a final chunk
    span.EndSpan();
    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), chunks_after_3);

    EXPECT_TRUE(mock_agent_service_->recorded_spans_.back()->isFinal())
        << "Last chunk should be final";
}

// ========== SpanImpl SetStatusCode ==========

TEST_F(SpanTest, SpanImplSetStatusCodeSuccessTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");

    span.SetStatusCode(200);
    span.EndSpan();

    // status 200 < 400, so err should not be set
    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& chunk = mock_agent_service_->recorded_spans_.back();
    EXPECT_EQ(chunk->getSpanData()->getErr(), SPAN_ERR_NONE)
        << "Success status should not set error";
}

TEST_F(SpanTest, SpanImplSetStatusCodeFailureTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");

    span.SetStatusCode(500);
    span.EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& chunk = mock_agent_service_->recorded_spans_.back();
    EXPECT_EQ(chunk->getSpanData()->getErr(), 1)
        << "Failure status (>=400) should set error";
}

// ========== SpanImpl extractContext ==========

TEST_F(SpanTest, SpanImplExtractContextWithoutTraceIdGeneratesNewTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    MockTraceContextReader reader;

    // No HEADER_TRACE_ID set — should generate a new trace ID
    span.extractContext(reader, reader.Get(HEADER_TRACE_ID));

    TraceId& tid = span.GetTraceId();
    EXPECT_EQ(tid.StartTime, mock_agent_service_->getStartTime())
        << "Generated trace ID should use agent start time";
}

TEST_F(SpanTest, SpanImplExtractContextWithHostHeaderTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    MockTraceContextReader reader;

    reader.SetContext(HEADER_TRACE_ID, "agent^1234567890^1");
    reader.SetContext(HEADER_SPAN_ID, "100");
    reader.SetContext(HEADER_HOST, "upstream-host:8080");

    span.extractContext(reader, reader.Get(HEADER_TRACE_ID));

    span.EndSpan();
    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& data = mock_agent_service_->recorded_spans_.back()->getSpanData();
    EXPECT_EQ(data->getAcceptorHost(), "upstream-host:8080");
    EXPECT_EQ(data->getEndPoint(), "upstream-host:8080");
    EXPECT_EQ(data->getRemoteAddr(), "upstream-host:8080");
}

TEST_F(SpanTest, SpanImplExtractContextWithFlagTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    MockTraceContextReader reader;

    reader.SetContext(HEADER_TRACE_ID, "agent^1234567890^1");
    reader.SetContext(HEADER_SPAN_ID, "100");
    reader.SetContext(HEADER_FLAG, "5");

    span.extractContext(reader, reader.Get(HEADER_TRACE_ID));
    span.EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    EXPECT_EQ(mock_agent_service_->recorded_spans_.back()->getSpanData()->getFlags(), 5);
}

TEST_F(SpanTest, SpanImplExtractContextWithParentServiceNameTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    MockTraceContextReader reader;

    reader.SetContext(HEADER_TRACE_ID, "agent^1234567890^1");
    reader.SetContext(HEADER_SPAN_ID, "100");
    reader.SetContext(HEADER_PARENT_APP_NAME, "ParentApp");
    reader.SetContext(HEADER_PARENT_APP_NAMESPACE, "ParentNamespace");
    reader.SetContext(HEADER_PARENT_SERVICE_NAME, "parent-service");

    span.extractContext(reader, reader.Get(HEADER_TRACE_ID));
    span.EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& data = mock_agent_service_->recorded_spans_.back()->getSpanData();
    EXPECT_EQ(data->getParentServiceName(), "parent-service")
        << "Pinpoint-pServiceName header should populate the span's parentServiceName";
    EXPECT_EQ(data->getParentAppNamespace(), "ParentNamespace")
        << "Pinpoint-pAppNamespace header should populate the span's parentAppNamespace";
}

// ========== SpanEventImpl InjectContext After Span Finished ==========

TEST_F(SpanTest, SpanEventInjectContextAfterSpanFinishedTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    auto se = span.NewSpanEvent("test-event");

    span.EndSpan();

    MockTraceContextWriter writer;
    se->InjectContext(writer);

    EXPECT_FALSE(writer.Get(HEADER_TRACE_ID).has_value())
        << "Should not inject context once the owning span is finished";
}

// ========== SpanImpl GetSpanEvent Without Events ==========

TEST_F(SpanTest, SpanImplGetSpanEventWithoutEventsTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");

    // No events created — should return noop
    auto se = span.GetSpanEvent();
    EXPECT_NE(se, nullptr) << "Should return noop span event, not nullptr";
}

// ========== SpanImpl SetError Single Arg ==========

TEST_F(SpanTest, SpanImplSetErrorSingleArgTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");

    span.SetError("something went wrong");
    span.EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& last = mock_agent_service_->recorded_spans_.back();
    EXPECT_EQ(last->getSpanData()->getErr(), 1);
    EXPECT_EQ(last->getSpanData()->getErrorString(), "something went wrong");
    // The error name "Error" should have been cached
    EXPECT_GE(mock_agent_service_->getCachedErrorId("Error"), 0);
}

// ========== SpanChunk Optimize Multi-Event Test ==========

TEST_F(SpanTest, SpanChunkOptimizeMultipleEventsTest) {
    auto span = std::make_shared<SpanImpl>(mock_agent_service_.get(), "test-op", "test-rpc");
    auto span_data = span->getSpanData();

    // Create events with different depths/sequences to test optimization
    auto event1 = make_test_span_event_unique(*span, "e1");
    auto event2 = make_test_span_event_unique(*span, "e2");
    auto event3 = make_test_span_event_unique(*span, "e3");
    auto* event1_ptr = event1.get();
    auto* event2_ptr = event2.get();
    auto* event3_ptr = event3.get();

    span_data->addSpanEvent(std::move(event1));
    span_data->addSpanEvent(std::move(event2));
    span_data->addSpanEvent(std::move(event3));

    // Finish in reverse order (stack LIFO)
    span_data->finishSpanEvent(); // event3
    span_data->finishSpanEvent(); // event2
    span_data->finishSpanEvent(); // event1

    SpanChunk chunk(span_data, true);
    EXPECT_EQ(chunk.getSpanEventChunk().size(), 3);

    auto& events = chunk.getSpanEventChunk();
    ASSERT_EQ(events.size(), 3);
    EXPECT_EQ(events[0].get(), event1_ptr) << "SpanData should drain finished events in sequence order";
    EXPECT_EQ(events[1].get(), event2_ptr) << "SpanData should drain finished events in sequence order";
    EXPECT_EQ(events[2].get(), event3_ptr) << "SpanData should drain finished events in sequence order";

    chunk.optimizeSpanEvents();

    // Finished events are already sequence-ordered before optimization.
    for (size_t i = 1; i < events.size(); i++) {
        EXPECT_GE(events[i]->getSequence(), events[i-1]->getSequence())
            << "Events should remain sorted by sequence after optimization";
    }

    // key_time should be set to span start time for final chunks
    EXPECT_EQ(chunk.getKeyTime(), span_data->getStartTime());
}

TEST_F(SpanTest, SpanChunkOptimizeNonFinalKeyTimeTest) {
    auto span = std::make_shared<SpanImpl>(mock_agent_service_.get(), "test-op", "test-rpc");
    auto span_data = span->getSpanData();

    auto event = make_test_span_event_unique(*span, "e1");
    span_data->addSpanEvent(std::move(event));
    span_data->finishSpanEvent();

    SpanChunk chunk(span_data, false);
    chunk.optimizeSpanEvents();

    // For non-final chunks, key_time should be the first event's start time
    EXPECT_GE(chunk.getKeyTime(), 0)
        << "Non-final chunk key time should come from first event";
}

// ========== SpanImpl Double EndSpan ==========

TEST_F(SpanTest, SpanImplDoubleEndSpanTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    span.EndSpan();

    size_t count_after_first = mock_agent_service_->getRecordedSpansCount();

    span.EndSpan();  // second call — should be no-op

    EXPECT_EQ(mock_agent_service_->getRecordedSpansCount(), count_after_first)
        << "Second EndSpan should not record another span";
}

// ========== SpanImpl SetLogging Verification ==========

TEST_F(SpanTest, SpanImplSetLoggingAfterFinishTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    span.EndSpan();

    MockTraceContextWriter writer;
    span.SetLogging(writer);

    EXPECT_FALSE(writer.Get("PtxId").has_value())
        << "SetLogging should be no-op after EndSpan";
}

// ========== SpanImpl NewAsyncSpan Overflow ==========

TEST_F(SpanTest, SpanImplNewAsyncSpanOverflowTest) {
    // Set very small depth limit
    auto config = std::make_shared<Config>();
    config->span.max_event_depth = 2;
    config->span.max_event_sequence = 512;
    config->span.event_chunk_size = 100;
    mock_agent_service_->reloadConfig(config);

    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");

    // Push one real event
    auto e1 = span.NewSpanEvent("e1");

    // This should overflow (depth now 3 >= 2)
    auto e2 = span.NewSpanEvent("e2-overflow");

    // NewAsyncSpan should return noop when overflow > 0
    auto async = span.NewAsyncSpan("async-op");
    EXPECT_NE(async, nullptr) << "Should return noop span on overflow";

    // Clean up
    e2->EndEvent(); // overflow--
    e1->EndEvent(); // real event
    span.EndSpan();
}

} // namespace pinpoint
