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
#include <cstdint>
#include <limits>
#include <map>
#include <thread>
#include <functional>
#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "../src/span.h"
#include "../src/logging.h"
#include "../src/config.h"
#include "../src/agent_service.h"
#include "../src/url_stat.h"
#include "../src/grpc_builders.h"
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

// Drives SpanImpl::extractContext() the way AgentImpl::NewSpan does: parse the
// inbound HEADER_TRACE_ID when present (a continued trace), otherwise generate a
// fresh one and mark the trace as new so no upstream header is adopted.
static void extract_context(SpanImpl& span, AgentService& agent, TraceContextReader& reader) {
    const auto hdr = reader.Get(HEADER_TRACE_ID);
    span.extractContext(reader,
                        hdr.has_value() ? TraceId::parseTraceId(hdr.value()) : agent.generateTraceId(),
                        hdr.has_value());
}

// ========== SpanData Tests ==========

TEST_F(SpanTest, SpanDataConstructorTest) {
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-operation");
    
    EXPECT_EQ(span_data.getOperationName(), "")
        << "the name is not stored a second time once its API ID identifies it";
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
    span_data->finishSpanEvent(span_data->topSpanEvent());
    EXPECT_EQ(span_data->getFinishedEventsCount(), 1) << "Should have 1 finished event";

    span_data->finishSpanEvent(span_data->topSpanEvent());
    EXPECT_EQ(span_data->getFinishedEventsCount(), 2) << "Should have 2 finished events";

    // Take finished events (moves them out, leaving the vector empty)
    std::vector<SpanEventImpl*> taken;
    span_data->takeFinishedEvents(taken);
    ASSERT_EQ(taken.size(), 2) << "Should have taken 2 finished events";
    EXPECT_EQ(taken[0], event1_ptr) << "Finished events should be returned in sequence order";
    EXPECT_EQ(taken[1], event2_ptr) << "Finished events should be returned in sequence order";
    EXPECT_EQ(span_data->getFinishedEventsCount(), 0) << "Finished events should be cleared";
}

TEST_F(SpanTest, SpanDataTraceIdParsingTest) {
    // A well-formed trace id parses into a populated, non-empty() TraceId.
    const TraceId tid = TraceId::parseTraceId("test-agent-001^1234567890^42");
    EXPECT_FALSE(tid.empty());
    EXPECT_EQ(tid.agentId(), "test-agent-001");
    EXPECT_EQ(tid.StartTime, 1234567890);
    EXPECT_EQ(tid.Sequence, 42);
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
    span_data->finishOpenSpanEvents();

    SpanChunk chunk(span_data, false);
    
    EXPECT_FALSE(chunk.isFinal()) << "Should not be final chunk";
    EXPECT_GT(chunk.getSpanEventChunk().size(), 0) << "Should have span events";
}

TEST_F(SpanTest, SpanChunkEndPointSnapshotTest) {
    auto span_data = make_test_span_data_ptr(*mock_agent_service_, "test-operation");
    span_data->setEndPoint("host-a:8080");

    // A non-final chunk is serialized on the gRPC worker while the span is
    // still live, so it must carry its own endpoint snapshot: a later
    // SetEndPoint on the owning thread must not affect (or race with) the
    // chunk being serialized.
    SpanChunk chunk(span_data, false);
    span_data->setEndPoint("host-b:9090");

    EXPECT_EQ(chunk.getEndPoint(), "host-a:8080")
        << "Chunk should keep the endpoint snapshot taken at creation";
    EXPECT_EQ(span_data->getEndPoint(), "host-b:9090")
        << "Span data should hold the updated endpoint";
}

TEST_F(SpanTest, SpanChunkDestructorReleasesRetiredPayloadTest) {
    auto span = std::make_shared<SpanImpl>(mock_agent_service_.get(), "test-operation", "test-rpc");
    auto span_data = span->getSpanData();

    auto event = make_test_span_event_unique(*span, "payload-event");
    auto* event_ptr = event.get();
    span_data->addSpanEvent(std::move(event));
    event_ptr->SetEndPoint("db-host:3306");
    event_ptr->SetDestination("MySQL");
    event_ptr->SetError("SomeError", "boom");
    event_ptr->SetAnnotation(12, "annotation-value");
    span_data->finishSpanEvent(span_data->topSpanEvent());

    const auto sequence = event_ptr->getSequence();
    {
        SpanChunk chunk(span_data, false);
        // Until the chunk is destroyed (i.e. until the gRPC worker has
        // consumed it), the payload must stay readable for serialization.
        EXPECT_EQ(event_ptr->getEndPoint(), "db-host:3306");
        EXPECT_EQ(event_ptr->getDestinationId(), "MySQL");
        EXPECT_EQ(event_ptr->getErrorString(), "boom");
        EXPECT_FALSE(event_ptr->getAnnotations()->getAnnotations().empty());
    }

    // Chunk destroyed: the heavy payload is released while the event object
    // itself stays alive in SpanData's retired list (a tombstone at a stable
    // address for user-held raw SpanEventPtr handles).
    EXPECT_TRUE(event_ptr->getOperationName().empty())
        << "operation should be released once the chunk is done";
    EXPECT_TRUE(event_ptr->getEndPoint().empty())
        << "endpoint should be released once the chunk is done";
    EXPECT_TRUE(event_ptr->getDestinationId().empty())
        << "destination should be released once the chunk is done";
    EXPECT_TRUE(event_ptr->getErrorString().empty())
        << "error string should be released once the chunk is done";
    EXPECT_TRUE(event_ptr->getAnnotations()->getAnnotations().empty())
        << "annotation list should be released once the chunk is done";
    EXPECT_EQ(event_ptr->getSequence(), sequence)
        << "numeric identity survives the payload release";
}

TEST_F(SpanTest, SpanEventHandleSafeAfterPayloadReleaseTest) {
    // The user-facing flow: a raw SpanEventPtr retained past EndEvent must
    // stay a safe warn/no-op handle even after the chunk that carried the
    // event has been destroyed and the payload released. Under ASan this
    // also proves the tombstone is a live object, not freed memory.
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    auto se = span.NewSpanEvent("retained-op");
    se->SetEndPoint("host:80");
    se->EndEvent();
    span.EndSpan();

    // Destroy the recorded chunks, which releases the retired payload.
    mock_agent_service_->recorded_spans_.clear();

    se->EndEvent();                    // duplicate end: warn no-op
    se->SetOperationName("too-late");  // mutators: finished_-guarded no-ops
    se->SetEndPoint("too-late:81");
    se->SetAnnotation(12, 42);         // finished_-guarded no-op
}

// ========== SpanImpl Tests ==========

TEST_F(SpanTest, SpanImplConstructorTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    
    EXPECT_TRUE(span.IsSampled()) << "Span should be sampled";
    EXPECT_NE(span.getSpanData()->getAnnotations(), nullptr) << "Annotations should be available";
    // Span ID might be 0 initially until context is extracted or generated
    EXPECT_GE(span.GetSpanId(), 0) << "Span ID should be non-negative";
}

TEST_F(SpanTest, SpanConfigSnapshotUsesTheSpansResolvedConfigGeneration) {
    mock_agent_service_->setAppName("resolved-app");
    mock_agent_service_->setAppType(7777);
    mock_agent_service_->setServiceName("resolved-service");
    mock_agent_service_->publishConfig([](Config& config) {
        config.revision = 1;
        config.span.max_event_depth = 7;
        config.span.max_event_sequence = 11;
        config.http.server.rec_request_header = {"X-First"};
        config.http.client.rec_response_header = {"X-Client"};
        config.sql.trace_bind_value = true;
    });

    SpanImpl first(mock_agent_service_.get(), "first", "/first");
    EXPECT_EQ(first.GetConfigRevision(), 1);
    const auto first_config = first.GetConfigSnapshot();
    EXPECT_EQ(first_config.revision, 1);
    EXPECT_EQ(first_config.application_name, "resolved-app");
    EXPECT_EQ(first_config.application_type, 7777);
    EXPECT_EQ(first_config.service_name, "resolved-service");
    EXPECT_EQ(first_config.max_event_depth, 7);
    EXPECT_EQ(first_config.max_event_sequence, 11);
    EXPECT_EQ(first_config.http_server_headers[HTTP_REQUEST],
              std::vector<std::string>{"X-First"});
    EXPECT_EQ(first_config.http_client_headers[HTTP_RESPONSE],
              std::vector<std::string>{"X-Client"});
    EXPECT_TRUE(first_config.sql_trace_bind_value);

    mock_agent_service_->publishConfig([](Config& config) {
        config.revision = 2;
        config.span.max_event_depth = 17;
        config.span.max_event_sequence = 23;
        config.http.server.rec_request_header = {"X-Reloaded"};
        config.sql.trace_bind_value = false;
    });

    // An existing span keeps the generation (and revision) it was admitted under.
    EXPECT_EQ(first.GetConfigRevision(), 1);
    EXPECT_EQ(first.GetConfigSnapshot().max_event_depth, 7);
    EXPECT_TRUE(first.GetConfigSnapshot().sql_trace_bind_value);
    EXPECT_EQ(first.GetConfigSnapshot().http_server_headers[HTTP_REQUEST],
              std::vector<std::string>{"X-First"});

    SpanImpl reloaded(mock_agent_service_.get(), "reloaded", "/reloaded");
    EXPECT_EQ(reloaded.GetConfigRevision(), 2);
    const auto reloaded_config = reloaded.GetConfigSnapshot();
    EXPECT_EQ(reloaded_config.revision, 2);
    EXPECT_EQ(reloaded_config.max_event_depth, 17);
    EXPECT_EQ(reloaded_config.max_event_sequence, 23);
    EXPECT_EQ(reloaded_config.http_server_headers[HTTP_REQUEST],
              std::vector<std::string>{"X-Reloaded"});
    EXPECT_FALSE(reloaded_config.sql_trace_bind_value);
}

TEST_F(SpanTest, SpanConfigSnapshotDefaultsBindValueCaptureOff) {
    // A default-constructed snapshot carries no resolved config, so the capture
    // flag has to read false there even though Config defaults it to true.
    EXPECT_FALSE(SpanConfigSnapshot{}.sql_trace_bind_value);
    EXPECT_TRUE(Config{}.sql.trace_bind_value);
}

TEST_F(SpanTest, SpanImplCompoundAnnotationTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");

    // The public composite overload (the ANNOTATION_HTTP_PROXY_HEADER payload
    // shape) must land as a LONG_INT_INT_BYTE_BYTE_STRING annotation.
    span.SetAnnotation(ANNOTATION_HTTP_PROXY_HEADER,
                       int64_t{1755678900123}, 2, 150, 10, 20, "backend");

    auto& annotations = span.getSpanData()->getAnnotations()->getAnnotations();
    ASSERT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, ANNOTATION_HTTP_PROXY_HEADER);
    ASSERT_EQ(pair.second.type(), ANNOTATION_TYPE_LONG_INT_INT_BYTE_BYTE_STRING);
    auto& value = std::get<LongIntIntByteByteStringValue>(pair.second.data);
    EXPECT_EQ(value.longValue, 1755678900123);
    EXPECT_EQ(value.intValue1, 2);
    EXPECT_EQ(value.intValue2, 150);
    EXPECT_EQ(value.byteValue1, 10);
    EXPECT_EQ(value.byteValue2, 20);
    EXPECT_EQ(value.stringValue, "backend");
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

// Wrapper-recorded events (batch replay) carry caller-supplied position and
// timing: the recorded chunk must keep them verbatim, and elapsed must come
// from the preset end time instead of the wall clock.
TEST_F(SpanTest, RecordSpanEventReplaysPositionAndTimingTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");

    const int64_t start_ms = 1700000000000;
    const int64_t end_ms = start_ms + 250;
    auto event = span.RecordSpanEvent("replayed-op", 2100, 3, 2,
                                      start_ms, end_ms, 77);
    ASSERT_NE(event, nullptr);
    // Already finished on return; a wrapper still calling EndEvent() must hit
    // the duplicate-end no-op instead of double-ending the event.
    event->EndEvent();
    span.EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& events = mock_agent_service_->recorded_spans_.back()->getSpanEventChunk();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0]->getSequence(), 3) << "caller-assigned sequence kept";
    EXPECT_EQ(events[0]->getDepth(), 2) << "caller-assigned depth kept";
    EXPECT_EQ(events[0]->getServiceType(), 2100);
    EXPECT_EQ(events[0]->getStartTime(), start_ms) << "caller-recorded start kept";
    EXPECT_EQ(events[0]->getEndElapsed(), 250)
        << "elapsed must come from the preset end time, not the wall clock";
    EXPECT_EQ(events[0]->getAsyncId(), 77) << "async link flushed with the event";
}

// The configured max depth/sequence backstop drops out-of-range records with
// a shared no-op event; the rest of the batch still records.
TEST_F(SpanTest, RecordSpanEventEnforcesLimitsTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");

    const int64_t now_ms = 1700000000000;
    auto over_seq = span.RecordSpanEvent("dropped", 2100, 100000, 1,
                                         now_ms, now_ms, NONE_ASYNC_ID);
    over_seq->EndEvent();  // no-op on the shared noop event
    auto over_depth = span.RecordSpanEvent("dropped", 2100, 0, 100000,
                                           now_ms, now_ms, NONE_ASYNC_ID);
    over_depth->EndEvent();
    auto kept = span.RecordSpanEvent("kept", 2100, 0, 1,
                                     now_ms, now_ms, NONE_ASYNC_ID);
    kept->EndEvent();
    span.EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    EXPECT_EQ(mock_agent_service_->recorded_spans_.back()->getSpanEventChunk().size(), 1u)
        << "records past the limits are dropped, the valid one is kept";
}

// A replayed event is complete on arrival, so RecordSpanEvent finalizes it
// before returning: the depth reservation must be released by the call itself,
// never held until an EndEvent a wrapper may never send — a leak there would
// walk the span into early max_event_depth overflow.
TEST_F(SpanTest, RecordSpanEventReleasesDepthImmediatelyTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    auto& data = *span.getSpanData();
    // Baseline of an idle span: depth 1, nothing open.
    const auto baseline = data.getEventDepth();
    ASSERT_EQ(baseline, 1);

    const int64_t now_ms = 1700000000000;
    constexpr int32_t kEvents = 5;  // below event_chunk_size (10): one chunk
    for (int32_t i = 0; i < kEvents; ++i) {
        auto event = span.RecordSpanEvent("replayed", 2100, i, 1,
                                          now_ms, now_ms + 1, NONE_ASYNC_ID);
        ASSERT_NE(event, nullptr);
        // No EndEvent() anywhere in this loop.
        EXPECT_EQ(data.getEventDepth(), baseline)
            << "depth reserved by record #" << i << " was not returned";
        EXPECT_EQ(data.topSpanEvent(), nullptr)
            << "record #" << i << " was left open on the event stack";
    }

    span.EndSpan();
    EXPECT_EQ(data.getEventDepth(), baseline) << "depth counter must not drift";

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& events = mock_agent_service_->recorded_spans_.back()->getSpanEventChunk();
    EXPECT_EQ(events.size(), static_cast<size_t>(kEvents))
        << "every record still reaches the chunk without an EndEvent";
    for (const auto* event : events) {
        EXPECT_EQ(event->getEndElapsed(), 1)
            << "each record keeps its own caller-supplied end time";
    }
}

// The caller-id NewAsyncSpan overload must not require a native top event:
// wrappers that batch their events have none while the span is live.
TEST_F(SpanTest, NewAsyncSpanWithCallerIdsTest) {
    auto span = std::make_shared<SpanImpl>(
        mock_agent_service_.get(), "test-operation", "test-rpc");

    auto async_span = span->NewAsyncSpan("bg-op", 42, 3);
    ASSERT_NE(async_span, nullptr);
    auto* impl = dynamic_cast<SpanImpl*>(async_span.get());
    ASSERT_NE(impl, nullptr)
        << "must hand out a real span even with an empty native event stack";
    EXPECT_EQ(impl->getSpanData()->getAsyncId(), 42);
    EXPECT_EQ(impl->getSpanData()->getAsyncSequence(), 3);
    EXPECT_EQ(impl->getSpanData()->getSpanId(), span->GetSpanId())
        << "async child records under the parent's span id";
    EXPECT_NE(impl->getSpanData()->topSpanEvent(), nullptr)
        << "the root async event is created as in the classic overload";

    async_span->EndSpan();
    span->EndSpan();
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

// Ending a parent event while its child is still open must not finish the
// child with the parent's end call and strand the parent on the stack: the
// stack is unwound down to the event actually being ended, and a later
// EndEvent on the implicitly-finished child stays a no-op.
TEST_F(SpanTest, SpanEventEndEventOutOfOrderUnwindsTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");

    auto outer = span.NewSpanEvent("outer");
    auto inner = span.NewSpanEvent("inner");
    (void)inner;

    outer->EndEvent(); // out of order: implicitly finishes "inner" too

    EXPECT_EQ(span.getSpanData()->topSpanEvent(), nullptr)
        << "Both events should be off the stack after the out-of-order end";

    inner->EndEvent(); // already implicitly finished: must be a no-op

    span.EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    EXPECT_EQ(mock_agent_service_->recorded_spans_.back()->getSpanEventChunk().size(), 2u)
        << "Both events should be recorded exactly once";
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
    EXPECT_EQ(events[0]->getApiId(),
              mock_agent_service_->getCachedApiId("event-ended-by-handle"))
        << "the recorded event is identified by the API ID its name resolved to";
}

TEST_F(SpanTest, SpanImplEndSpanTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");

    // End the span
    span.EndSpan();

    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), 0) << "Span should be recorded";
}

// Events user code failed to end must be finished implicitly at EndSpan and
// still land in the final chunk instead of being silently dropped.
TEST_F(SpanTest, EndSpanFinishesOpenSpanEventsTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");

    auto outer = span.NewSpanEvent("outer");
    auto inner = span.NewSpanEvent("inner");
    (void)outer;
    (void)inner;

    span.EndSpan(); // neither event was ended

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    EXPECT_EQ(mock_agent_service_->recorded_spans_.back()->getSpanEventChunk().size(), 2u)
        << "Unended events must be finished implicitly and recorded";
}

// An async span's root event stays open until EndSpan by design. A leftover
// (unended) child event must not be finished IN PLACE OF the root: both are
// drained and recorded.
TEST_F(SpanTest, AsyncSpanEndSpanFinishesLeftoverChildEventsTest) {
    SpanImpl parent(mock_agent_service_.get(), "parent-operation", "parent-rpc");

    auto prepare_event = parent.NewSpanEvent("prepare-async");
    auto async_span = parent.NewAsyncSpan("async-task");
    ASSERT_NE(async_span, nullptr);

    auto child = async_span->NewSpanEvent("async-child");
    (void)child; // never ended by user code

    async_span->EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    EXPECT_EQ(mock_agent_service_->recorded_spans_.back()->getSpanEventChunk().size(), 2u)
        << "Async root event and its leftover child must both be recorded";

    prepare_event->EndEvent();
    parent.EndSpan();
}

// When the per-span exception buffer is full, the dropped exception must not
// leave behind an ANNOTATION_EXCEPTION_ID referencing an id that is never sent.
TEST_F(SpanTest, ExceptionBufferFullSkipsExceptionIdAnnotationTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    MockCallStackReader reader;
    reader.AddFrame("module", "function", "file.cpp", 42);

    const auto count_exception_ids = [](SpanEventImpl* event) {
        size_t count = 0;
        for (const auto& [key, data] : event->getAnnotations()->getAnnotations()) {
            if (key == ANNOTATION_EXCEPTION_ID) {
                count++;
            }
        }
        return count;
    };

    span.NewSpanEvent("failing-event");
    auto* se = span.getSpanData()->topSpanEvent();
    ASSERT_NE(se, nullptr);

    // Fill the buffer to its cap (SpanImpl::kMaxBufferedExceptions). All of
    // them are one cause chain on this event, so the id is annotated once.
    constexpr size_t kMaxBufferedExceptions = 100;
    for (size_t i = 0; i < kMaxBufferedExceptions; i++) {
        se->SetError("Error", "boom", reader);
    }
    EXPECT_EQ(count_exception_ids(se), 1u)
        << "One chain on one event annotates its exception id once";

    // A different event starts its own chain, but the buffer is full, so the
    // exception is dropped and must leave no id behind.
    span.NewSpanEvent("dropped-event");
    auto* dropped = span.getSpanData()->topSpanEvent();
    ASSERT_NE(dropped, nullptr);
    dropped->SetError("Error", "one-too-many", reader);
    EXPECT_EQ(count_exception_ids(dropped), 0u)
        << "A dropped exception must not add an exception-id annotation";

    dropped->EndEvent();
    se->EndEvent();
    span.EndSpan();
}

// system_clock can step backwards (NTP); elapsed must be clamped, never
// negative. Simulated by forcing a start time in the future.
TEST_F(SpanTest, ElapsedClampedToNonNegativeTest) {
    const auto future = std::chrono::system_clock::now() + std::chrono::seconds(10);

    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    span.SetStartTime(future);

    auto event = span.NewSpanEvent("test-event");
    event->SetStartTime(future);
    event->EndEvent();
    span.EndSpan();

    EXPECT_EQ(span.getSpanData()->getElapsed(), 0)
        << "Span elapsed must be clamped to zero when the clock steps backwards";
    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    const auto& events = mock_agent_service_->recorded_spans_.back()->getSpanEventChunk();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0]->getEndElapsed(), 0)
        << "Event elapsed must be clamped to zero when the clock steps backwards";
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

// GetTraceId(), a span event's InjectContext() and SetLogging() all read the
// same cached wire form (getTraceIdWire()); a continued trace must surface
// identically on all three. Regression guard for routing them through the one
// cache: the trace id is constant across a distributed trace, so the outbound
// header must carry the parent's exact id (only the span id changes downstream).
TEST_F(SpanTest, TraceIdWireConsistentAcrossSurfacesTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    MockTraceContextReader reader;
    reader.SetContext(HEADER_TRACE_ID, "consistent-agent^1700000000^99");
    extract_context(span, *mock_agent_service_, reader);

    const std::string expected = "consistent-agent^1700000000^99";

    // 1) GetTraceId()
    EXPECT_EQ(span.GetTraceId(), expected) << "GetTraceId should return the wire form";

    // 2) Outbound InjectContext() through a span event.
    MockTraceContextWriter inject_writer;
    auto se = span.NewSpanEvent("outbound-call");
    se->InjectContext(inject_writer);
    ASSERT_TRUE(inject_writer.Get(HEADER_TRACE_ID).has_value());
    EXPECT_EQ(inject_writer.Get(HEADER_TRACE_ID).value(), expected)
        << "InjectContext must emit the same wire trace id";

    // 3) SetLogging() writes the same wire id under the log MDC key.
    MockTraceContextWriter log_writer;
    span.SetLogging(log_writer);
    ASSERT_TRUE(log_writer.Get("PtxId").has_value());
    EXPECT_EQ(log_writer.Get("PtxId").value(), expected)
        << "SetLogging must emit the same wire trace id";

    se->EndEvent();
    span.EndSpan();
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

// A Java receiver with profiler.cluster.namespace set runs DefaultNameSpaceChecker,
// which accepts only an absent header or an exact match — an empty Pinpoint-pAppNamespace
// fails the equals() and RequestTraceReader starts newTrace() instead of continuing,
// severing the trace at the C++ -> Java hop. The agent has no namespace setting, so the
// header must never be written; every other header keeps going out unchanged.
TEST_F(SpanTest, SpanEventInjectContextOmitsEmptyNamespaceAndHostTest) {
    mock_agent_service_->setServiceName("my-service");

    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    auto se = span.NewSpanEvent("test-event");
    se->SetDestination("downstream:8080");

    MockTraceContextWriter writer;
    se->InjectContext(writer);

    EXPECT_FALSE(writer.Get(HEADER_PARENT_APP_NAMESPACE).has_value())
        << "Pinpoint-pAppNamespace must be omitted, not sent as an empty string";

    for (const auto header : {HEADER_TRACE_ID, HEADER_SPAN_ID, HEADER_PARENT_SPAN_ID,
                              HEADER_FLAG, HEADER_PARENT_APP_NAME, HEADER_PARENT_APP_TYPE,
                              HEADER_PARENT_SERVICE_NAME, HEADER_HOST}) {
        EXPECT_TRUE(writer.Get(header).has_value())
            << "header must still be propagated: " << header;
    }
    EXPECT_EQ(writer.Get(HEADER_HOST).value(), "downstream:8080");

    se->EndEvent();
    span.EndSpan();
}

// Java writes Pinpoint-Host only when the host is non-null; an event with no
// destination must leave the header out rather than send "".
TEST_F(SpanTest, SpanEventInjectContextOmitsHostWhenDestinationEmptyTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    auto se = span.NewSpanEvent("test-event"); // no SetDestination()

    MockTraceContextWriter writer;
    se->InjectContext(writer);

    EXPECT_FALSE(writer.Get(HEADER_HOST).has_value())
        << "Pinpoint-Host must be omitted when the span event has no destination";
    EXPECT_TRUE(writer.Get(HEADER_TRACE_ID).has_value())
        << "the rest of the context still propagates";

    se->EndEvent();
    span.EndSpan();
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
    
    extract_context(span, *mock_agent_service_, reader);
    
    // Verify context was extracted with correct values
    EXPECT_EQ(span.GetSpanId(), expected_span_id) << "Span ID should match the value from context";
    
    // Verify trace ID was parsed correctly. GetTraceId() now returns the wire
    // string, so inspect the decomposed fields through the internal SpanData.
    const TraceId& trace_id = span.getSpanData()->getTraceId();
    EXPECT_EQ(trace_id.StartTime, 1234567890) << "Trace ID start time should be parsed correctly";
    EXPECT_EQ(trace_id.Sequence, 42) << "Trace ID sequence should be parsed correctly";
    EXPECT_EQ(span.GetTraceId(), "test-agent-001^1234567890^42")
        << "GetTraceId should return the wire-form trace id";
}

TEST_F(SpanTest, SpanImplNewAsyncSpanTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");

    // Give the parent a known trace/span identity so we can assert the async
    // child inherits it (rather than trivially comparing default zeros).
    MockTraceContextReader reader;
    reader.SetContext(HEADER_TRACE_ID, "test-agent^1700000000^11");
    reader.SetContext(HEADER_SPAN_ID, "555");
    extract_context(span, *mock_agent_service_, reader);

    // Create a span event first for context (required by NewAsyncSpan).
    auto base_event = span.NewSpanEvent("base-event");

    auto async_span = span.NewAsyncSpan("async-operation");
    ASSERT_NE(async_span, nullptr) << "Async span should be created";
    // A real (non-noop) async span inherits the parent's span id and trace id.
    EXPECT_EQ(async_span->GetSpanId(), span.GetSpanId())
        << "Async child should inherit the parent span id";
    EXPECT_EQ(async_span->GetTraceId(), span.GetTraceId())
        << "Async child should inherit the parent trace id";

    async_span->EndSpan();
    base_event->EndEvent();
    span.EndSpan();
}

// Regression: exceptions captured on an async span must be flushed when the
// async span ends. EndSpan's async branch previously skipped sendExceptions(),
// so a span event's ANNOTATION_EXCEPTION_ID referenced exception metadata that
// was never sent to the collector, losing the captured call stack.
TEST_F(SpanTest, AsyncSpanFlushesExceptionsOnEndTest) {
    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");

    MockTraceContextReader reader;
    reader.SetContext(HEADER_TRACE_ID, "test-agent^1700000000^11");
    reader.SetContext(HEADER_SPAN_ID, "555");
    extract_context(span, *mock_agent_service_, reader);

    auto base_event = span.NewSpanEvent("base-event");
    auto async_span = span.NewAsyncSpan("async-operation");
    ASSERT_NE(async_span, nullptr);

    // Capture an exception (with call stack) on the async span.
    auto async_event = async_span->NewSpanEvent("async-child");
    ASSERT_NE(async_event, nullptr);
    MockCallStackReader callstack_reader;
    callstack_reader.AddFrame("/usr/lib/libmyapp.so", "handler", "/src/async.cpp", 7);
    async_event->SetError("AsyncError", "boom", callstack_reader);
    async_event->EndEvent();

    const int before = mock_agent_service_->recorded_exceptions_;
    async_span->EndSpan();  // must flush the async span's captured exceptions
    EXPECT_EQ(mock_agent_service_->recorded_exceptions_, before + 1)
        << "Async span EndSpan must send captured exceptions";

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
    extract_context(parent_span, *mock_agent_service_, parent_reader);

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
    
    extract_context(child_span, *mock_agent_service_, reader);
    
    // Both spans should have valid IDs (after context extraction)
    EXPECT_NE(parent_span.GetSpanId(), 0) << "Parent span should have non-zero ID";
    EXPECT_NE(child_span.GetSpanId(), 0) << "Child span should have non-zero ID";
    EXPECT_NE(parent_span.GetSpanId(), child_span.GetSpanId()) << "Span IDs should be different";
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
    extract_context(parent, *mock_agent_service_, reader);

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
        // GetSpanEvent() is owner-checked too, so it must accept the thread
        // that legally owns the span (a Debug build asserts if it does not).
        EXPECT_EQ(async_span->GetSpanEvent(), se)
            << "the owning thread must reach its own innermost event";
        se->EndEvent();
        async_span->EndSpan();
    });
    worker.join();

    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), recorded_before)
        << "Async span should record its own chunk from the worker thread";
    EXPECT_EQ(async_span->GetSpanId(), parent.GetSpanId())
        << "Async child should inherit the parent span id";
    EXPECT_EQ(async_span->GetTraceId(), parent.GetTraceId())
        << "Async child should inherit the parent trace id";

    prepare_event->EndEvent();
    parent.EndSpan();
}

#ifdef NDEBUG
// GetSpanEvent() reads the same event stack NewSpanEvent writes, so it is
// bound to the owning thread just as tightly; before this check it was the one
// public entry point that let a foreign thread reach the stack (and, while
// overflowed, the shared disabled event) with no warning at all.
//
// Release-only: the same check asserts in a Debug build, which is the point of
// the assert — a test cannot deliberately trip it there.
TEST_F(SpanTest, GetSpanEventFromForeignThreadWarnsTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    seed_test_trace_id(span, *mock_agent_service_);
    // Binds the span to this thread.
    auto se = span.NewSpanEvent("owned-event");

    // The default sink is stdout, so no logger state has to be swapped in and
    // back out around this.
    std::cout.flush();
    testing::internal::CaptureStdout();
    std::thread foreign([&]() { (void)span.GetSpanEvent(); });
    foreign.join();
    std::cout.flush();
    const auto logged = testing::internal::GetCapturedStdout();

    EXPECT_NE(logged.find("must be used by a single thread"), std::string::npos)
        << "GetSpanEvent from a foreign thread must report the violation; got: " << logged;

    se->EndEvent();
    span.EndSpan();
}
#endif  // NDEBUG

// ========== parseTraceId Edge Case Tests ==========
//
// parseTraceId() returns an empty() TraceId on any structural failure; NewSpan
// then hands back a noop span rather than recording a trace with no agent id.

TEST_F(SpanTest, ParseTraceIdMissingSeparatorTest) {
    // No '^' at all — parse fails and yields an empty trace id.
    const TraceId tid = TraceId::parseTraceId("no-separator-here");
    EXPECT_TRUE(tid.empty()) << "a trace id with no separator should parse to empty";
}

TEST_F(SpanTest, ParseTraceIdOnlyOneSeparatorTest) {
    // Only one '^' — the sequence field is missing, so the parse fails.
    const TraceId tid = TraceId::parseTraceId("agent^12345");
    EXPECT_TRUE(tid.empty()) << "a trace id missing the sequence field should parse to empty";
}

TEST_F(SpanTest, ParseTraceIdAgentIdTooLongTest) {
    // AgentId exceeds kMaxAgentIdLength (24) — parse fails.
    const TraceId tid = TraceId::parseTraceId("this-agent-id-is-way-too-long^1234567890^1");
    EXPECT_TRUE(tid.empty()) << "a trace id with an over-long agent id should parse to empty";
}

TEST_F(SpanTest, ParseTraceIdEmptyFieldsTest) {
    // "^^" has all separators but every field empty. Accepting it would
    // record a live trace at ("", 0, 0) on which every such malformed header
    // collides, so it is rejected like the other malformations.
    const TraceId tid = TraceId::parseTraceId("^^");
    EXPECT_TRUE(tid.empty()) << "empty fields should parse to an empty trace id";
}

TEST_F(SpanTest, ParseTraceIdStartTimeTooLongTest) {
    // The StartTime field is bounded to kMaxInt64StringLength (20). A 21-char
    // field is rejected outright (separate guard from the agent-id length check),
    // yielding an empty trace id so NewSpan drops to a noop span.
    const TraceId tid = TraceId::parseTraceId("agent^123456789012345678901^1");
    EXPECT_TRUE(tid.empty()) << "an over-long StartTime field should parse to empty";
}

TEST_F(SpanTest, ParseTraceIdSequenceTooLongTest) {
    // The Sequence field has its own length guard (kMaxInt64StringLength = 20).
    const TraceId tid = TraceId::parseTraceId("agent^123^123456789012345678901");
    EXPECT_TRUE(tid.empty()) << "an over-long Sequence field should parse to empty";
}

TEST_F(SpanTest, ParseTraceIdExtraFieldsRejectedTest) {
    // The wire form is exactly agentId^startTime^sequence (two separators). A
    // header with a further '^' — extra fields or a trailing separator — is
    // rejected structurally, mirroring the missing-separator guard. Without this
    // the surplus is absorbed into the Sequence field, fails to parse, and
    // degrades to sequence 0, recording a bogus trace on which every distinct
    // malformed header collides at (agentId, startTime, 0).
    const TraceId extra = TraceId::parseTraceId("a^1^2^3");
    EXPECT_TRUE(extra.empty()) << "a trace id with more than two separators should parse to empty";

    const TraceId trailing = TraceId::parseTraceId("a^1^2^");
    EXPECT_TRUE(trailing.empty()) << "a trace id with a trailing separator should parse to empty";
}

TEST_F(SpanTest, ParseTraceIdAgentIdLengthBoundaryTest) {
    // kMaxAgentIdLength is 24 and the guard is a strict `pos1 > 24`, so an agent
    // id of exactly 24 chars is accepted while 25 is rejected. Pins the exact
    // off-by-one contract that the existing (29-char) too-long test cannot.
    const std::string id24(24, 'a');
    const TraceId at_limit = TraceId::parseTraceId(id24 + "^100^1");
    EXPECT_FALSE(at_limit.empty()) << "a 24-char agent id is at the limit and valid";
    EXPECT_EQ(at_limit.agentId(), id24);

    const std::string id25(25, 'a');
    const TraceId over_limit = TraceId::parseTraceId(id25 + "^100^1");
    EXPECT_TRUE(over_limit.empty()) << "a 25-char agent id exceeds the limit and is rejected";
}

TEST_F(SpanTest, ParseTraceIdNumericOverflowRejectedTest) {
    // A numeric field that is within the length guard (<= 20 chars) but
    // overflows int64 fails stoll_ and is rejected structurally: absorbing it
    // as 0 would record a live trace at (agent, 0, seq) on which every such
    // header collides.
    const TraceId tid = TraceId::parseTraceId("agent^99999999999999999999^7");
    EXPECT_TRUE(tid.empty()) << "an int64-overflowing StartTime should parse to empty";
}

// ========== TraceId::toString Tests ==========
//
// toString() serializes the trace id to its wire form with to_chars into a fixed
// stack buffer (no ostringstream); it runs on every outbound InjectContext() /
// SetLogging(), so its edge cases are worth pinning directly.

TEST_F(SpanTest, TraceIdToStringRoundTripTest) {
    // toString() is the inverse of parseTraceId() for a canonical wire id.
    const std::string wire = "round-trip-agent^1234567890^42";
    EXPECT_EQ(TraceId::parseTraceId(wire).toString(), wire)
        << "parseTraceId then toString should round-trip a canonical id";
}

TEST_F(SpanTest, TraceIdToStringEmptyAgentIdTest) {
    // A present-but-empty agent id (no longer producible via parseTraceId,
    // which rejects "^^"; constructed directly here) renders the empty
    // leading field, not a dropped separator.
    const TraceId tid("", 0, 0);
    ASSERT_FALSE(tid.empty());
    EXPECT_EQ(tid.toString(), "^0^0");
}

TEST_F(SpanTest, TraceIdToStringHandlesInt64ExtremesTest) {
    // Guards toString()'s fixed `char num[20]` stack buffer: the widest int64_t
    // (INT64_MIN is 20 chars including the sign) must serialize without
    // truncation. A parseable-but-negative inbound header can reach here.
    const TraceId tid("x", std::numeric_limits<int64_t>::max(),
                           std::numeric_limits<int64_t>::min());
    EXPECT_EQ(tid.toString(), "x^9223372036854775807^-9223372036854775808")
        << "toString must serialize int64 extremes without truncation";
}

// ========== SpanData getTraceIdWire (wire-form cache) Tests ==========
//
// getTraceIdWire() lazily serializes the trace id to its wire form and caches
// the result; setTraceId() invalidates that cache. injectContext(), SetLogging()
// and GetTraceId() all read this one cached string on the span-owning thread.

TEST_F(SpanTest, GetTraceIdWireSerializesTraceIdTest) {
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-operation");
    span_data.setTraceId(TraceId("wire-agent", 1700000000, 7));

    EXPECT_EQ(span_data.getTraceIdWire(), "wire-agent^1700000000^7")
        << "wire form should be agentId^startTime^sequence";
    // A second read returns the cached value; it must still equal a fresh
    // toString() of the same trace id.
    EXPECT_EQ(span_data.getTraceIdWire(), span_data.getTraceId().toString())
        << "cached wire form must equal a direct toString()";
}

TEST_F(SpanTest, GetTraceIdWireEmptyForDefaultTraceIdTest) {
    // make_test_span_data leaves the trace id default (empty()); the wire form
    // must stay an empty string rather than serialize a bogus "^0^0".
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-operation");
    ASSERT_TRUE(span_data.getTraceId().empty()) << "precondition: default trace id is empty";
    EXPECT_TRUE(span_data.getTraceIdWire().empty())
        << "wire form of an empty trace id should be an empty string";
}

TEST_F(SpanTest, GetTraceIdWireInvalidatedOnSetTraceIdTest) {
    // The cache-invalidation contract: reading the wire form caches it, and a
    // later setTraceId() must serve the NEW id, not the stale cached string.
    // Every other test sets the trace id exactly once, so this is the only guard
    // on setTraceId()'s trace_id_wire_.clear().
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-operation");

    span_data.setTraceId(TraceId("agent-a", 100, 1));
    EXPECT_EQ(span_data.getTraceIdWire(), "agent-a^100^1");  // caches "agent-a^100^1"

    span_data.setTraceId(TraceId("agent-b", 200, 2));
    EXPECT_EQ(span_data.getTraceIdWire(), "agent-b^200^2")
        << "setTraceId must invalidate the cached wire form";
}

// ========== SpanData finishSpanEvent on Empty Stack ==========

TEST_F(SpanTest, SpanDataFinishSpanEventOnEmptyStackTest) {
    SpanData span_data = make_test_span_data(*mock_agent_service_, "test-op");

    // Should not crash — logs a warning, finished_events unchanged
    // (topSpanEvent() is nullptr on an empty stack)
    span_data.finishSpanEvent(span_data.topSpanEvent());
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

    // Config max_event_depth is 64, and the allowance is max + 1 (Java
    // DefaultCallStack parity, see is_event_overflow in span.cpp): depth
    // 1..65 are recorded.
    std::vector<SpanEventPtr> events;
    for (int i = 0; i < 65; i++) {
        auto se = span.NewSpanEvent("event-" + std::to_string(i));
        events.push_back(se);
    }

    // Next event should overflow (depth 66 is past the 65-level allowance)
    auto overflow_event = span.NewSpanEvent("overflow-event");
    EXPECT_NE(overflow_event, nullptr) << "Should return disabled event on overflow";

    // Ending the disabled event should decrement the overflow counter,
    // not pop from the stack
    overflow_event->EndEvent();

    // Now ending the real events should work
    for (int i = 64; i >= 0; i--) {
        events[i]->EndEvent();
    }

    span.EndSpan();
    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), 0);
}

// MaxEventDepth allows max + 1 nesting levels, the same as Java's
// DefaultCallStack (isOverflow() compares `maxDepth < index` against the
// pre-push element count): MaxEventDepth=3 records events at depth 1..4, and
// only the fifth nesting level is discarded.
TEST_F(SpanTest, SpanImplEventDepthAllowsMaxPlusOneLevelsTest) {
    auto config = std::make_shared<Config>();
    config->span.max_event_depth = 3;
    config->span.max_event_sequence = 512;
    config->span.event_chunk_size = 100;
    mock_agent_service_->reloadConfig(config);

    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");

    std::vector<SpanEventPtr> events;
    for (int i = 0; i < 4; i++) {
        auto* se = span.NewSpanEvent("depth-" + std::to_string(i + 1));
        ASSERT_NE(se, noopSpanEvent()) << "depth " << (i + 1) << " must be recorded";
        events.push_back(se);
    }

    auto* overflowed = span.NewSpanEvent("depth-5");
    EXPECT_EQ(overflowed, span.GetSpanEvent())
        << "the 5th nesting level must hand out the shared disabled event";
    EXPECT_NE(overflowed, events[3]) << "the 5th nesting level must not be recorded";
    overflowed->EndEvent();

    for (int i = 3; i >= 0; i--) {
        events[i]->EndEvent();
    }
    span.EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& chunk = mock_agent_service_->recorded_spans_.back()->getSpanEventChunk();
    ASSERT_EQ(chunk.size(), 4u) << "exactly the events at depth 1..4 are recorded";
    EXPECT_EQ(chunk[0]->getDepth(), 1);
    EXPECT_EQ(chunk[1]->getDepth(), 2);
    EXPECT_EQ(chunk[2]->getDepth(), 3);
    EXPECT_EQ(chunk[3]->getDepth(), 4);
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
    extract_context(span, *mock_agent_service_, reader);

    // max 2 allows three nesting levels (max + 1, Java DefaultCallStack parity).
    auto outer = span.NewSpanEvent("outer-event");           // depth 1
    auto middle = span.NewSpanEvent("middle-event");         // depth 2
    auto real = span.NewSpanEvent("real-event");             // depth 3, the last real one
    auto overflowed = span.NewSpanEvent("overflowed-event"); // depth 4 -> overflow
    EXPECT_NE(overflowed, noopSpanEvent())
        << "Overflow should hand out the disabled event, not the plain noop";
    EXPECT_EQ(span.GetSpanEvent(), overflowed)
        << "GetSpanEvent during overflow should return the disabled event";

    // Recording is a no-op and must not crash.
    overflowed->SetDestination("downstream:8080");
    overflowed->SetError("ignored");
    overflowed->SetAnnotation(12, 42);

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
    middle->EndEvent();
    outer->EndEvent();
    span.EndSpan();

    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), 0);
}

// The single shared DisabledSpanEvent per span has no per-instance finished_
// flag; the overflow_ counter is the ONLY guard against a duplicate/extra end.
// endDisabledSpanEvent() must (a) count multiple overflows, (b) balance them one
// per end, and (c) treat an end beyond the outstanding overflows as a warned
// no-op that never falls through to pop a real event off the stack.
TEST_F(SpanTest, DisabledSpanEventOverEndingIsGuardedTest) {
    auto config = std::make_shared<Config>();
    // The allowance is max + 1, so max_event_depth=2 admits exactly three
    // nested real events; the fourth NewSpanEvent overflows.
    config->span.max_event_depth = 2;
    config->span.max_event_sequence = 512;
    config->span.event_chunk_size = 100;
    mock_agent_service_->reloadConfig(config);

    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    MockTraceContextReader reader;
    extract_context(span, *mock_agent_service_, reader);

    auto* outer = span.NewSpanEvent("outer-event");           // depth 1 -> real
    auto* middle = span.NewSpanEvent("middle-event");         // depth 2 -> real
    auto* real = span.NewSpanEvent("real-event");             // depth 3 -> real, depth -> 4
    auto* overflow1 = span.NewSpanEvent("overflow-1");        // depth 4 -> overflow_=1
    auto* overflow2 = span.NewSpanEvent("overflow-2");        // still overflowed -> overflow_=2
    EXPECT_EQ(overflow1, overflow2)
        << "overflowed events reuse the one shared DisabledSpanEvent instance";
    EXPECT_NE(overflow1, real);

    // Two outstanding overflows: the disabled event is handed out until both end.
    EXPECT_EQ(span.GetSpanEvent(), overflow1) << "still overflowed after zero ends";
    overflow2->EndEvent();                                   // overflow_ 2 -> 1
    EXPECT_EQ(span.GetSpanEvent(), overflow1) << "still overflowed with one end left";
    overflow1->EndEvent();                                   // overflow_ 1 -> 0
    EXPECT_EQ(span.GetSpanEvent(), real)
        << "both overflows balanced: the real top event is active again";

    // Over-end: end the disabled event more times than it overflowed. Each extra
    // call must be a no-op guarded by overflow_ == 0, NOT a pop of the real event.
    overflow1->EndEvent();
    overflow1->EndEvent();
    EXPECT_EQ(span.GetSpanEvent(), real)
        << "over-ending the disabled event must not pop the still-active real event";

    real->EndEvent();
    middle->EndEvent();
    outer->EndEvent();
    span.EndSpan();
    EXPECT_GT(mock_agent_service_->getRecordedSpansCount(), 0);
}

// Lifetime parity between the two event kinds (ASan): SpanData outlives its
// SpanImpl whenever a chunk is still in flight, and user code may hold both a
// real SpanEventPtr and the shared overflow placeholder that long. Calling
// either one after the span is destroyed must be a guarded no-op, not a
// use-after-free — the disabled event used to dereference the dead span.
TEST_F(SpanTest, EventsOutlivingTheirSpanAreGuardedTest) {
    auto config = std::make_shared<Config>();
    config->span.max_event_depth = 2;   // three real events (max + 1), then overflow
    config->span.max_event_sequence = 512;
    config->span.event_chunk_size = 100;
    mock_agent_service_->reloadConfig(config);

    auto span = std::make_unique<SpanImpl>(mock_agent_service_.get(), "test-op", "test-rpc");
    MockTraceContextReader reader;
    reader.SetContext(HEADER_TRACE_ID, "test-agent^1700000000^42");
    reader.SetContext(HEADER_SPAN_ID, "555");
    extract_context(*span, *mock_agent_service_, reader);

    span->NewSpanEvent("outer-event");                          // depth 1
    span->NewSpanEvent("middle-event");                         // depth 2
    auto* real = span->NewSpanEvent("real-event");              // depth 3, the last real one
    auto* overflowed = span->NewSpanEvent("overflowed-event");  // depth 4 -> overflow

    // Pin the span data the way a chunk in flight on the gRPC span worker
    // does, then drop the span while both events are still open.
    auto data = span->getSpanData();
    span.reset();

    EXPECT_EQ(data->getOwner(), nullptr) << "the span unlinks itself on destruction";

    // Every path that used to reach through to the span: no crash, no ASan
    // report, nothing recorded.
    MockTraceContextWriter writer;
    overflowed->SetDestination("downstream:8080");
    overflowed->InjectContext(writer);
    EXPECT_EQ(writer.Get(HEADER_SAMPLED).value(), "s0")
        << "a dead span cannot produce a context; downstream must not trace";
    EXPECT_FALSE(writer.Get(HEADER_TRACE_ID).has_value());
    overflowed->EndEvent();
    overflowed->EndEvent();  // over-ending stays guarded too

    MockTraceContextWriter real_writer;
    // These two reach the agent, not just the span, and the agent is only
    // pinned by the span (SpanImpl::agent_ref_) — so they have to be gated on
    // the span being alive just like every other cross-object access.
    const auto errors_before = mock_agent_service_->cached_errors_.size();
    const auto headers_before = mock_agent_service_->recorded_client_headers_;
    real->SetError("late", "after the span is gone");
    MockHeaderReader header_reader;
    header_reader.SetHeader("X-Late", "after the span is gone");
    real->RecordHeader(HTTP_REQUEST, header_reader);
    EXPECT_EQ(mock_agent_service_->cached_errors_.size(), errors_before)
        << "SetError must not reach the agent once the span that pinned it is gone";
    EXPECT_EQ(mock_agent_service_->recorded_client_headers_, headers_before)
        << "RecordHeader must not reach the agent once the span that pinned it is gone";

    real->SetAnnotation(12, 42);
    real->InjectContext(real_writer);
    EXPECT_EQ(real_writer.Get(HEADER_SAMPLED).value(), "s0");
    EXPECT_FALSE(real_writer.Get(HEADER_TRACE_ID).has_value());
    real->EndEvent();
    real->EndEvent();

    EXPECT_EQ(data->getFinishedEventsCount(), 0u)
        << "an event ended after its span is gone reaches no chunk";
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

TEST_F(SpanTest, SpanImplDuplicateEndEventAfterChunkFlushTest) {
    auto config = std::make_shared<Config>();
    config->span.max_event_depth = 64;
    config->span.max_event_sequence = 512;
    config->span.event_chunk_size = 1;
    mock_agent_service_->reloadConfig(config);

    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");

    auto event = span.NewSpanEvent("flushed-event");
    event->EndEvent();
    ASSERT_EQ(mock_agent_service_->getRecordedSpansCount(), 1)
        << "Ending the event should trigger an intermediate chunk flush";

    // The event was handed to the chunk, but SpanData retains ownership of
    // flushed events, so the duplicate-EndEvent no-op documented in
    // pinpoint/tracer.h must stay safe instead of touching freed memory.
    event->EndEvent();

    span.EndSpan();
    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    EXPECT_EQ(mock_agent_service_->recorded_spans_.back()->getSpanEventChunk().size(), 0u)
        << "Duplicate EndEvent must not add or re-send any event";
}

TEST_F(SpanTest, SpanDroppedWithoutEndSpanReleasesActiveSpanTest) {
    auto& stats = mock_agent_service_->getAgentStats();
    const int64_t now_ms = 1000000;

    {
        auto span = std::make_shared<SpanImpl>(mock_agent_service_.get(), "op", "rpc");
        // Register through the production path: extractContext links the
        // span's embedded ActiveSpanNode, the same node the destructor
        // backstop must unlink.
        MockTraceContextReader reader;
        extract_context(*span, *mock_agent_service_, reader);

        int32_t counts[4] = {0, 0, 0, 0};
        stats.collectActiveRequests(counts, now_ms);
        EXPECT_EQ(counts[0] + counts[1] + counts[2] + counts[3], 1)
            << "Span should be registered as active";
        // Dropped here WITHOUT EndSpan — an early-return/exception path.
    }

    int32_t counts[4] = {0, 0, 0, 0};
    stats.collectActiveRequests(counts, now_ms);
    EXPECT_EQ(counts[0] + counts[1] + counts[2] + counts[3], 0)
        << "Destroying a span without EndSpan must still release its active-span entry";
}

TEST_F(SpanTest, FinishedSpanDestructorDoesNotReenterAgentStatsTest) {
    class CountingMockAgentService final : public MockAgentService {
    public:
        AgentStats& getAgentStats() override {
            ++agent_stats_accesses_;
            return MockAgentService::getAgentStats();
        }

        int agent_stats_accesses_{0};
    } service;

    auto span = std::make_shared<SpanImpl>(&service, "op", "rpc");
    MockTraceContextReader reader;
    extract_context(*span, service, reader);
    span->EndSpan();

    const auto accesses_after_end = service.agent_stats_accesses_;
    span.reset();

    EXPECT_EQ(service.agent_stats_accesses_, accesses_after_end)
        << "an unlinked node must not make the destructor touch a non-owning agent";
}

TEST_F(SpanTest, SpanImplKeepsAgentServiceAliveTest) {
    class SharedMockAgentService : public MockAgentService,
                                   public std::enable_shared_from_this<SharedMockAgentService> {
    public:
        std::shared_ptr<AgentService> selfRef() noexcept override {
            return weak_from_this().lock();
        }
    };

    auto service = std::make_shared<SharedMockAgentService>();
    std::weak_ptr<AgentService> observer = service;

    auto span = std::make_shared<SpanImpl>(service.get(), "test-op", "test-rpc");
    service.reset();

    EXPECT_FALSE(observer.expired())
        << "A live span must keep the agent service alive (UnsampledSpan parity)";

    span->EndSpan();
    span.reset();
    EXPECT_TRUE(observer.expired())
        << "Releasing the span must release the agent keep-alive";
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
    extract_context(span, *mock_agent_service_, reader);

    const TraceId& tid = span.getSpanData()->getTraceId();
    EXPECT_EQ(tid.StartTime, mock_agent_service_->getStartTime())
        << "Generated trace ID should use agent start time";
}

// A span id header that does not parse used to leave the span id at its 0
// default, which is indistinguishable from a real id on the wire — every such
// request would be linked under the same span. Treat it as no id at all.
TEST_F(SpanTest, SpanImplExtractContextWithUnparsableSpanIdGeneratesNewTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    MockTraceContextReader reader;

    reader.SetContext(HEADER_TRACE_ID, "agent^1234567890^1");
    reader.SetContext(HEADER_SPAN_ID, "not-a-number");

    extract_context(span, *mock_agent_service_, reader);

    const int64_t span_id = span.getSpanData()->getSpanId();
    EXPECT_NE(span_id, 0) << "An unparsable span id must not be left at the 0 default";
    EXPECT_NE(span_id, kNullSpanId) << "A generated span id must never be the NULL sentinel";
}

// InjectContext writes this span's id as the callee's parent span id, so the
// generated child id must differ from it (Java SpanId.nextSpanID).
TEST_F(SpanTest, SpanEventInjectContextChildSpanIdDiffersFromParentTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    MockTraceContextReader reader;
    reader.SetContext(HEADER_TRACE_ID, "agent^1234567890^1");
    reader.SetContext(HEADER_SPAN_ID, "555");
    reader.SetContext(HEADER_PARENT_SPAN_ID, "111");
    extract_context(span, *mock_agent_service_, reader);

    auto se = span.NewSpanEvent("test-event");
    MockTraceContextWriter writer;
    se->InjectContext(writer);

    ASSERT_TRUE(writer.Get(HEADER_SPAN_ID).has_value());
    EXPECT_EQ(writer.Get(HEADER_PARENT_SPAN_ID).value(), "555");
    EXPECT_NE(writer.Get(HEADER_SPAN_ID).value(), "555");
    EXPECT_NE(writer.Get(HEADER_SPAN_ID).value(), "111");
    EXPECT_NE(writer.Get(HEADER_SPAN_ID).value(), "-1");
}

TEST_F(SpanTest, SpanImplExtractContextWithHostHeaderTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    MockTraceContextReader reader;

    reader.SetContext(HEADER_TRACE_ID, "agent^1234567890^1");
    reader.SetContext(HEADER_SPAN_ID, "100");
    reader.SetContext(HEADER_HOST, "upstream-host:8080");

    extract_context(span, *mock_agent_service_, reader);

    span.EndSpan();
    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& data = mock_agent_service_->recorded_spans_.back()->getSpanData();
    EXPECT_EQ(data->getAcceptorHost(), "upstream-host:8080");
    // Both default to the acceptor host without storing it again; the wire
    // must not be able to tell that only one copy was made.
    EXPECT_EQ(data->getEndPoint(), "upstream-host:8080");
    EXPECT_EQ(data->getRemoteAddr(), "upstream-host:8080");
}

// The acceptor-host default holds only until instrumentation supplies real
// values, and an explicitly empty value must stay empty rather than fall back
// to the host — the reason SpanData tracks "was it set" instead of testing
// the string for emptiness.
TEST_F(SpanTest, SpanImplExplicitEndpointOverridesAcceptorHostDefaultTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    MockTraceContextReader reader;

    reader.SetContext(HEADER_TRACE_ID, "agent^1234567890^1");
    reader.SetContext(HEADER_HOST, "upstream-host:8080");
    extract_context(span, *mock_agent_service_, reader);

    span.SetEndPoint("/orders");
    span.SetRemoteAddress("");

    span.EndSpan();
    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& data = mock_agent_service_->recorded_spans_.back()->getSpanData();
    EXPECT_EQ(data->getAcceptorHost(), "upstream-host:8080")
        << "the acceptor host itself is untouched by the overrides";
    EXPECT_EQ(data->getEndPoint(), "/orders")
        << "an explicit endpoint must replace the acceptor-host default";
    EXPECT_EQ(data->getRemoteAddr(), "")
        << "an explicitly empty remote address must stay empty, not "
           "resurrect the acceptor-host default";
}

TEST_F(SpanTest, SpanImplExtractContextWithFlagTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    MockTraceContextReader reader;

    reader.SetContext(HEADER_TRACE_ID, "agent^1234567890^1");
    reader.SetContext(HEADER_SPAN_ID, "100");
    reader.SetContext(HEADER_FLAG, "5");

    extract_context(span, *mock_agent_service_, reader);
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
    reader.SetContext(HEADER_PARENT_SERVICE_NAME, "parent-service");

    extract_context(span, *mock_agent_service_, reader);
    span.EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& data = mock_agent_service_->recorded_spans_.back()->getSpanData();
    EXPECT_EQ(data->getParentServiceName(), "parent-service")
        << "Pinpoint-pServiceName header should populate the span's parentServiceName";
}

// A request with no Pinpoint-TraceID starts a brand new trace, so the rest of
// the inbound Pinpoint headers belong to some *other* trace and must be ignored
// — adopting them produced a root span pointing at a parent that does not exist
// in this trace. Java gates the same block behind ServerRequestRecorder's
// `if (!recorder.isRoot())`.
TEST_F(SpanTest, SpanImplExtractContextWithoutTraceIdIgnoresUpstreamHeadersTest) {
    auto& stats = mock_agent_service_->getAgentStats();
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    MockTraceContextReader reader;

    // No HEADER_TRACE_ID — everything below is a leftover from another trace.
    reader.SetContext(HEADER_SPAN_ID, "555");
    reader.SetContext(HEADER_PARENT_SPAN_ID, "111");
    reader.SetContext(HEADER_PARENT_APP_NAME, "ParentApp");
    reader.SetContext(HEADER_PARENT_APP_TYPE, "1010");
    reader.SetContext(HEADER_PARENT_SERVICE_NAME, "parent-service");
    reader.SetContext(HEADER_FLAG, "5");
    reader.SetContext(HEADER_HOST, "upstream-host:8080");

    extract_context(span, *mock_agent_service_, reader);

    int32_t counts[4] = {0, 0, 0, 0};
    stats.collectActiveRequests(counts, 1000000);
    EXPECT_EQ(counts[0] + counts[1] + counts[2] + counts[3], 1)
        << "a new root trace must still register as an active span";

    span.EndSpan();
    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& data = mock_agent_service_->recorded_spans_.back()->getSpanData();
    EXPECT_EQ(data->getParentSpanId(), -1)
        << "a root span has no parent; the upstream pSpanID belongs to another trace";
    EXPECT_EQ(data->getParentAppName(), "")
        << "the upstream pAppName must not be recorded as this root span's caller";
    EXPECT_NE(data->getSpanId(), 555)
        << "the span id must be generated, not taken from another trace";
    EXPECT_NE(data->getParentAppType(), 1010);
    EXPECT_EQ(data->getParentServiceName(), "");
    EXPECT_EQ(data->getFlags(), 0);
    EXPECT_EQ(data->getAcceptorHost(), "")
        << "Pinpoint-Host is the non-root acceptor host; a new root trace has no acceptor";
}

// The mirror of the test above: a well-formed Pinpoint-TraceID means the trace
// really is continued, so the upstream headers are adopted as before.
TEST_F(SpanTest, SpanImplExtractContextWithTraceIdAdoptsUpstreamHeadersTest) {
    auto& stats = mock_agent_service_->getAgentStats();
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    MockTraceContextReader reader;

    reader.SetContext(HEADER_TRACE_ID, "agent^1234567890^1");
    reader.SetContext(HEADER_SPAN_ID, "555");
    reader.SetContext(HEADER_PARENT_SPAN_ID, "111");
    reader.SetContext(HEADER_PARENT_APP_NAME, "ParentApp");
    reader.SetContext(HEADER_PARENT_APP_TYPE, "1010");
    reader.SetContext(HEADER_HOST, "upstream-host:8080");

    extract_context(span, *mock_agent_service_, reader);

    int32_t counts[4] = {0, 0, 0, 0};
    stats.collectActiveRequests(counts, 1000000);
    EXPECT_EQ(counts[0] + counts[1] + counts[2] + counts[3], 1)
        << "a continued trace must register as an active span too";

    span.EndSpan();
    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& data = mock_agent_service_->recorded_spans_.back()->getSpanData();
    EXPECT_EQ(data->getSpanId(), 555);
    EXPECT_EQ(data->getParentSpanId(), 111);
    EXPECT_EQ(data->getParentAppName(), "ParentApp");
    EXPECT_EQ(data->getParentAppType(), 1010);
    EXPECT_EQ(data->getAcceptorHost(), "upstream-host:8080");
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

// An exception recorded only on a span event (DB/external call) must fail the
// whole transaction like Java: PSpan.err == 1 and the URL stat entry counts in
// the failed histogram, even with a 200 status.
TEST_F(SpanTest, SpanEventSetErrorMarksSpanAndUrlStatFailedTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    seed_test_trace_id(span, *mock_agent_service_);
    span.SetStatusCode(200);
    span.SetUrlStat("/api/users", "GET", 200);

    auto se = span.NewSpanEvent("db-query");
    se->SetError("SQLException", "connection refused");
    se->EndEvent();
    span.EndSpan();

    ASSERT_EQ(mock_agent_service_->recorded_url_stats_, 1);
    EXPECT_TRUE(mock_agent_service_->last_url_stat_failed_);

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto chunk = std::move(mock_agent_service_->recorded_spans_.back());
    google::protobuf::Arena arena;
    auto* pspan = build_grpc_span(std::move(chunk), &arena);
    ASSERT_NE(pspan, nullptr);
    EXPECT_EQ(pspan->err(), 1);

    // Same flag drives the failed histogram of the URL stat snapshot.
    UrlStatEntry entry("/api/users", "GET", 200);
    entry.elapsed_ = 10;
    entry.failed_ = mock_agent_service_->last_url_stat_failed_;
    entry.end_time_ = std::chrono::system_clock::now();
    UrlStatSnapshot snapshot;
    Config config;
    TickClock tick_clock(URL_STAT_TICK_INTERVAL.count());
    snapshot.add(&entry, config, tick_clock);
    auto& stats = snapshot.getEachStats();
    ASSERT_EQ(stats.size(), 1u);
    EXPECT_EQ(stats.begin()->second.fail.total(), 10) << "failed histogram must count the event-only error";
}

// The async counterpart of the test above, and a regression guard: an error
// recorded on an async child must fail the root transaction. An async span is
// serialized as a span chunk, which has no err field on the wire, so a flag
// left on the child's own SpanData reached neither PSpan.err nor the URL stat.
TEST_F(SpanTest, AsyncSpanEventSetErrorMarksRootSpanAndUrlStatFailedTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    seed_test_trace_id(span, *mock_agent_service_);
    span.SetStatusCode(200);
    span.SetUrlStat("/api/users", "GET", 200);

    auto base_event = span.NewSpanEvent("spawn-async");
    auto async_span = span.NewAsyncSpan("async-task");
    ASSERT_NE(async_span, nullptr);

    auto async_event = async_span->NewSpanEvent("db-query");
    ASSERT_NE(async_event, nullptr);
    async_event->SetError("SQLException", "connection refused");
    async_event->EndEvent();
    async_span->EndSpan();

    base_event->EndEvent();
    span.EndSpan();

    ASSERT_EQ(mock_agent_service_->recorded_url_stats_, 1);
    EXPECT_TRUE(mock_agent_service_->last_url_stat_failed_)
        << "an async child's error must fail the root's URL stat";

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    google::protobuf::Arena async_arena;
    auto* root_pspan = build_grpc_span(
        std::move(mock_agent_service_->recorded_spans_.back()), &async_arena);
    ASSERT_NE(root_pspan, nullptr);
    EXPECT_EQ(root_pspan->err(), 1)
        << "an async child's error must set the root PSpan.err";
}

// The real async shape: the child records its error on a worker thread and the
// root ends on the owning thread. The join orders the two here, which is why
// the assertion is deterministic; the shared flag is atomic because production
// callers have no such ordering (see SpanData::err_).
TEST_F(SpanTest, AsyncSpanErrorOnWorkerThreadFailsTheRootTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    seed_test_trace_id(span, *mock_agent_service_);
    span.SetStatusCode(200);

    auto base_event = span.NewSpanEvent("spawn-async");
    auto async_span = span.NewAsyncSpan("async-task");
    ASSERT_NE(async_span, nullptr);

    std::thread worker([&]() {
        auto se = async_span->NewSpanEvent("db-query");
        ASSERT_NE(se, nullptr);
        se->SetError("SQLException", "connection refused");
        se->EndEvent();
        async_span->EndSpan();
    });
    worker.join();

    base_event->EndEvent();
    span.EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    google::protobuf::Arena worker_arena;
    auto* root_pspan = build_grpc_span(
        std::move(mock_agent_service_->recorded_spans_.back()), &worker_arena);
    ASSERT_NE(root_pspan, nullptr);
    EXPECT_EQ(root_pspan->err(), 1)
        << "an error recorded on the worker thread must fail the root";
}

// The error flag is shared down a whole chain of async spans, not just one
// level: a grandchild's error must still reach the trace root.
TEST_F(SpanTest, NestedAsyncSpanErrorReachesTraceRootTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    seed_test_trace_id(span, *mock_agent_service_);
    span.SetStatusCode(200);

    auto base_event = span.NewSpanEvent("spawn-async");
    auto child = span.NewAsyncSpan("async-child");
    ASSERT_NE(child, nullptr);
    auto child_event = child->NewSpanEvent("spawn-grandchild");
    ASSERT_NE(child_event, nullptr);
    auto grandchild = child->NewAsyncSpan("async-grandchild");
    ASSERT_NE(grandchild, nullptr);

    auto leaf = grandchild->NewSpanEvent("db-query");
    ASSERT_NE(leaf, nullptr);
    leaf->SetError("SQLException", "connection refused");
    leaf->EndEvent();
    grandchild->EndSpan();
    child_event->EndEvent();
    child->EndSpan();
    base_event->EndEvent();
    span.EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    google::protobuf::Arena nested_arena;
    auto* root_pspan = build_grpc_span(
        std::move(mock_agent_service_->recorded_spans_.back()), &nested_arena);
    ASSERT_NE(root_pspan, nullptr);
    EXPECT_EQ(root_pspan->err(), 1)
        << "a nested async error must reach the trace root";
}

namespace {
    // Builds the PSpan the collector would see from the last recorded chunk.
    v1::PSpan* last_recorded_pspan(MockAgentService& agent, google::protobuf::Arena& arena) {
        if (agent.recorded_spans_.empty()) {
            return nullptr;
        }
        return build_grpc_span(std::move(agent.recorded_spans_.back()), &arena);
    }
}

// A span event with no operation name has neither an api id nor a name to
// fall back on, so the API annotation would go out with an empty value — the
// collector renders that as a blank api instead of the caller's service type.
// Go skips the fallback for an empty operationName and Java's
// AbstractRecorder.recordApi records nothing for a null descriptor.
TEST_F(SpanTest, EmptyOperationNameSendsNoApiAnnotationTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    seed_test_trace_id(span, *mock_agent_service_);

    auto named = span.NewSpanEvent("named-event");
    named->EndEvent();
    auto unnamed = span.NewSpanEvent("");
    unnamed->EndEvent();
    span.EndSpan();

    google::protobuf::Arena arena;
    auto* pspan = last_recorded_pspan(*mock_agent_service_, arena);
    ASSERT_NE(pspan, nullptr);
    ASSERT_EQ(pspan->spanevent_size(), 2);

    for (const auto& se : pspan->spanevent()) {
        for (const auto& annotation : se.annotation()) {
            if (annotation.key() == ANNOTATION_API) {
                EXPECT_FALSE(annotation.value().stringvalue().empty())
                    << "an API annotation with no value is worse than none";
            }
        }
    }

    const auto& unnamed_event = pspan->spanevent(1);
    EXPECT_EQ(unnamed_event.apiid(), 0) << "an empty operation caches no api id";
    for (const auto& annotation : unnamed_event.annotation()) {
        EXPECT_NE(annotation.key(), ANNOTATION_API)
            << "the nameless event must send no API annotation at all";
    }
}

// An async child span is created with an empty operation on purpose, so the
// span-level fallback hit the same way — on every async span, not just an edge
// case. Its PSpan must carry no API annotation either.
TEST_F(SpanTest, AsyncSpanSendsNoEmptyApiAnnotationTest) {
    SpanImpl parent(mock_agent_service_.get(), "parent-op", "parent-rpc");
    seed_test_trace_id(parent, *mock_agent_service_);

    auto prepare = parent.NewSpanEvent("prepare-async");
    auto async_span = parent.NewAsyncSpan("async-task");
    ASSERT_NE(async_span, nullptr);
    auto se = async_span->NewSpanEvent("thread-event");
    se->EndEvent();
    async_span->EndSpan();

    google::protobuf::Arena arena;
    auto* pspan = last_recorded_pspan(*mock_agent_service_, arena);
    ASSERT_NE(pspan, nullptr);
    EXPECT_EQ(pspan->apiid(), 0) << "an async child span caches no api id";
    for (const auto& annotation : pspan->annotation()) {
        EXPECT_NE(annotation.key(), ANNOTATION_API)
            << "an async span has no operation name, so it has no API to annotate";
    }

    prepare->EndEvent();
    parent.EndSpan();
}

// The third path into the same flag: an event created past MaxEventDepth /
// MaxEventSequence is handed the shared disabled event, which records nothing.
// Nothing recorded must not mean nothing failed — overflow caps profiling
// detail, not the verdict (Java's traceBlockBegin past the limit still hands
// back a recorder that reaches the trace root).
TEST_F(SpanTest, OverflowedSpanEventSetErrorMarksSpanAndUrlStatFailedTest) {
    auto config = std::make_shared<Config>();
    config->span.max_event_depth = 1;
    config->span.max_event_sequence = 512;
    config->span.event_chunk_size = 100;
    mock_agent_service_->reloadConfig(config);

    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    seed_test_trace_id(span, *mock_agent_service_);
    span.SetStatusCode(200);
    span.SetUrlStat("/api/users", "GET", 200);

    // max_event_depth=1 allows two nesting levels (max + 1).
    auto outer = span.NewSpanEvent("outer");
    auto inner = span.NewSpanEvent("inner");
    auto overflowed = span.NewSpanEvent("db-query");
    ASSERT_EQ(overflowed, span.GetSpanEvent()) << "depth 3 must hand out the disabled event";

    overflowed->SetError("SQLException", "connection refused");
    overflowed->EndEvent();
    inner->EndEvent();
    outer->EndEvent();
    span.EndSpan();

    ASSERT_EQ(mock_agent_service_->recorded_url_stats_, 1);
    EXPECT_TRUE(mock_agent_service_->last_url_stat_failed_)
        << "an error on an overflowed event must fail the URL stat";

    google::protobuf::Arena arena;
    auto* pspan = last_recorded_pspan(*mock_agent_service_, arena);
    ASSERT_NE(pspan, nullptr);
    EXPECT_EQ(pspan->err(), 1);
    ASSERT_EQ(pspan->spanevent_size(), 2) << "the overflowed event itself is still discarded";
    EXPECT_FALSE(pspan->spanevent(0).has_exceptioninfo())
        << "only the two real events are recorded, and neither recorded an error";
    EXPECT_FALSE(pspan->spanevent(1).has_exceptioninfo())
        << "only the two real events are recorded, and neither recorded an error";
}

// ========== Span.IgnoreErrors Tests ==========

// The C++ counterpart of Java's profiler.ignore-error-handler: a matched error
// is still recorded as exceptionInfo, it just does not fail the transaction.
TEST_F(SpanTest, SpanIgnoreErrorsKeepsExceptionInfoWithoutErrTest) {
    mock_agent_service_->mutableConfig()->span.ignore_errors = {{"NotFound", ""}};

    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    seed_test_trace_id(span, *mock_agent_service_);
    span.SetError("NotFound", "no such user");
    span.EndSpan();

    google::protobuf::Arena arena;
    auto* pspan = last_recorded_pspan(*mock_agent_service_, arena);
    ASSERT_NE(pspan, nullptr);
    EXPECT_EQ(pspan->err(), 0) << "an ignored error must not fail the span";
    ASSERT_TRUE(pspan->has_exceptioninfo()) << "the error is still reported, only unmarked";
    EXPECT_EQ(pspan->exceptioninfo().stringvalue().value(), "no such user");
    EXPECT_GE(mock_agent_service_->getCachedErrorId("NotFound"), 0);
}

TEST_F(SpanTest, SpanIgnoreErrorsUnregisteredNameStillMarksErrTest) {
    mock_agent_service_->mutableConfig()->span.ignore_errors = {{"NotFound", ""}};

    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    seed_test_trace_id(span, *mock_agent_service_);
    span.SetError("SQLException", "connection refused");
    span.EndSpan();

    google::protobuf::Arena arena;
    auto* pspan = last_recorded_pspan(*mock_agent_service_, arena);
    ASSERT_NE(pspan, nullptr);
    EXPECT_EQ(pspan->err(), 1) << "a name outside the ignore list must still fail the span";
    EXPECT_TRUE(pspan->has_exceptioninfo());
}

// A rule with only message_contains matches any name, like Java's
// exception-message@contains matcher on its own.
TEST_F(SpanTest, SpanIgnoreErrorsMessageContainsTest) {
    mock_agent_service_->mutableConfig()->span.ignore_errors = {{"", "canceled by client"}};

    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    seed_test_trace_id(span, *mock_agent_service_);
    span.SetError("RuntimeError", "request canceled by client");
    span.EndSpan();

    google::protobuf::Arena arena;
    auto* pspan = last_recorded_pspan(*mock_agent_service_, arena);
    ASSERT_NE(pspan, nullptr);
    EXPECT_EQ(pspan->err(), 0);
    EXPECT_TRUE(pspan->has_exceptioninfo());
}

// Both SetError paths share shouldMarkError(), so the span-event path (which
// propagates into PSpan.err and the URL stat failure flag) honors it too.
TEST_F(SpanTest, SpanEventIgnoreErrorsSkipsErrMarkTest) {
    mock_agent_service_->mutableConfig()->span.ignore_errors = {{"NotFound", ""}};

    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    seed_test_trace_id(span, *mock_agent_service_);
    span.SetStatusCode(200);
    span.SetUrlStat("/api/users", "GET", 200);

    auto se = span.NewSpanEvent("db-query");
    se->SetError("NotFound", "no such row");
    se->EndEvent();
    span.EndSpan();

    ASSERT_EQ(mock_agent_service_->recorded_url_stats_, 1);
    EXPECT_FALSE(mock_agent_service_->last_url_stat_failed_);

    google::protobuf::Arena arena;
    auto* pspan = last_recorded_pspan(*mock_agent_service_, arena);
    ASSERT_NE(pspan, nullptr);
    EXPECT_EQ(pspan->err(), 0);
    ASSERT_EQ(pspan->spanevent_size(), 1);
    EXPECT_TRUE(pspan->spanevent(0).has_exceptioninfo())
        << "the event still carries the exception, only the span is unmarked";
}

// ========== PAcceptEvent UNKNOWN Defaults ==========

// Java SpanMessageMapper defaults an unset remoteAddr/endPoint to "UNKNOWN"
// (DEFAULT_REMOTE_ADDRESS / DEFAULT_END_POINT). A span with neither set --
// nor an acceptor host to fall back on -- must serialize the same way here.
TEST_F(SpanTest, AcceptEventDefaultsUnsetEndPointAndRemoteAddrToUnknownTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    seed_test_trace_id(span, *mock_agent_service_);
    span.EndSpan();

    google::protobuf::Arena arena;
    auto* pspan = last_recorded_pspan(*mock_agent_service_, arena);
    ASSERT_NE(pspan, nullptr);
    ASSERT_TRUE(pspan->has_acceptevent());
    EXPECT_EQ(pspan->acceptevent().endpoint(), "UNKNOWN");
    EXPECT_EQ(pspan->acceptevent().remoteaddr(), "UNKNOWN");
    EXPECT_EQ(pspan->acceptevent().rpc(), "test-rpc") << "rpc keeps its real value";
}

// The default must not shadow a value the caller did set, including one that
// only reaches the wire through the acceptor-host fallback in SpanData.
TEST_F(SpanTest, AcceptEventKeepsSetEndPointAndRemoteAddrTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    seed_test_trace_id(span, *mock_agent_service_);
    span.SetEndPoint("orders.internal:8443");
    span.SetRemoteAddress("192.0.2.10");
    span.EndSpan();

    google::protobuf::Arena arena;
    auto* pspan = last_recorded_pspan(*mock_agent_service_, arena);
    ASSERT_NE(pspan, nullptr);
    ASSERT_TRUE(pspan->has_acceptevent());
    EXPECT_EQ(pspan->acceptevent().endpoint(), "orders.internal:8443");
    EXPECT_EQ(pspan->acceptevent().remoteaddr(), "192.0.2.10");

    SpanImpl host_only(mock_agent_service_.get(), "test-op", "test-rpc");
    seed_test_trace_id(host_only, *mock_agent_service_);
    host_only.SetAcceptorHost("gateway.example.test");
    host_only.EndSpan();

    google::protobuf::Arena host_arena;
    auto* host_pspan = last_recorded_pspan(*mock_agent_service_, host_arena);
    ASSERT_NE(host_pspan, nullptr);
    ASSERT_TRUE(host_pspan->has_acceptevent());
    EXPECT_EQ(host_pspan->acceptevent().endpoint(), "gateway.example.test");
    EXPECT_EQ(host_pspan->acceptevent().remoteaddr(), "gateway.example.test");
}

TEST_F(SpanTest, SpanImplSetErrorAbbreviatesLongMessageTest) {
    SpanImpl span(mock_agent_service_.get(), "test-op", "test-rpc");
    const std::string msg(300, 'a');

    span.SetError("SQLException", msg);
    span.EndSpan();

    ASSERT_FALSE(mock_agent_service_->recorded_spans_.empty());
    auto& last = mock_agent_service_->recorded_spans_.back();
    EXPECT_EQ(last->getSpanData()->getErrorString(), abbreviateErrorString(msg))
        << "Error message should be capped like Java's StringUtils.abbreviate(msg, 256)";
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

    // Finish in reverse order (stack LIFO: event3, event2, event1)
    span_data->finishOpenSpanEvents();

    SpanChunk chunk(span_data, true);
    EXPECT_EQ(chunk.getSpanEventChunk().size(), 3);

    auto& events = chunk.getSpanEventChunk();
    ASSERT_EQ(events.size(), 3);
    EXPECT_EQ(events[0], event1_ptr) << "SpanData should drain finished events in sequence order";
    EXPECT_EQ(events[1], event2_ptr) << "SpanData should drain finished events in sequence order";
    EXPECT_EQ(events[2], event3_ptr) << "SpanData should drain finished events in sequence order";

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
    span_data->finishSpanEvent(span_data->topSpanEvent());

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

    // Push real events up to the allowance (max + 1 = three levels)
    auto e0 = span.NewSpanEvent("e0");
    auto e1 = span.NewSpanEvent("e1");
    auto e2 = span.NewSpanEvent("e2");

    // This should overflow (depth 4 is past the three-level allowance)
    auto e3 = span.NewSpanEvent("e3-overflow");

    // NewAsyncSpan should return noop when overflow > 0
    auto async = span.NewAsyncSpan("async-op");
    EXPECT_NE(async, nullptr) << "Should return noop span on overflow";

    // Clean up
    e3->EndEvent(); // overflow--
    e2->EndEvent(); // real event
    e1->EndEvent(); // real event
    e0->EndEvent(); // real event
    span.EndSpan();
}

// The API-misuse warnings are reproducible once per request on a
// misinstrumented host, so they are throttled per call site: repeating the same
// misuse must cost a bounded number of lines, not one line per call.
TEST_F(SpanTest, RepeatedApiMisuseIsThrottledToOneLinePerCallSite) {
    std::vector<std::string> lines;
    std::mutex lines_mutex;
    Logger::getInstance().setLogLevel("warning");
    Logger::getInstance().setSink([&](const char*, const char* message) {
        std::lock_guard<std::mutex> lock(lines_mutex);
        lines.emplace_back(message);
    });

    SpanImpl span(mock_agent_service_.get(), "test-operation", "test-rpc");
    auto event = span.NewSpanEvent("event");
    event->EndEvent();
    span.EndSpan();

    constexpr int kRepeats = 1000;
    for (int i = 0; i < kRepeats; ++i) {
        span.SetAnnotation(ANNOTATION_HTTP_URL, "after the end");   // span finished
        event->SetAnnotation(ANNOTATION_HTTP_URL, "after the end"); // event finished
        span.EndSpan();                                             // span finished
        event->EndEvent();                                          // event finished
    }

    Logger::getInstance().setSink({});
    Logger::getInstance().setLogLevel("info");

    // Four distinct call sites, each granting at most one line per 60s window,
    // and the window may already have been opened by an earlier test in this
    // binary — so the bound is what matters, not the exact count.
    EXPECT_LE(lines.size(), 4u)
        << 4 * kRepeats << " misuse calls must not produce a line each";
}

} // namespace pinpoint
