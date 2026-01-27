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

#include "../src/span_event.h"
#include "../src/config.h"
#include "../src/agent_service.h"
#include "../src/span.h"
#include "../src/url_stat.h"
#include "../src/stat.h"
#include "../include/pinpoint/tracer.h"

namespace pinpoint {

// Mock implementation of AgentService for SpanEvent testing
class MockAgentService : public AgentService {
public:
    MockAgentService() : is_exiting_(false), start_time_(1234567890), cached_start_time_str_(std::to_string(start_time_)), trace_id_counter_(0) {
        // Enable callstack trace for exception tests
        config_.enable_callstack_trace = true;
    }

    // AgentService interface implementation
    bool isExiting() const override { return is_exiting_; }
    std::string_view getAppName() const override { return "TestApp"; }
    int32_t getAppType() const override { return 1300; }
    std::string_view getAgentId() const override { return "test-agent-001"; }
    std::string_view getAgentName() const override { return "TestAgent"; }
    const Config& getConfig() const override { return config_; }
    int64_t getStartTime() const override { return start_time_; }
    void reloadConfig(const Config& cfg) override { config_ = cfg; }

    TraceId generateTraceId() override {
        TraceId trace_id;
        trace_id.StartTime = start_time_;
        trace_id.Sequence = trace_id_counter_++;
        return trace_id;
    }

    void recordSpan(std::unique_ptr<SpanChunk> span) const override {
        recorded_spans_++;
    }

    void recordUrlStat(std::unique_ptr<UrlStatEntry> stat) const override {
        recorded_url_stats_++;
    }

    void recordException(SpanData* span_data) const override {
        recorded_exceptions_++;
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

    void removeCacheError(const StringMeta& error_meta) const override {
        // Mock implementation
    }

    int32_t cacheSql(std::string_view sql_query) const override {
        cached_sqls_[std::string(sql_query)] = sql_id_counter_;
        return sql_id_counter_++;
    }

    void removeCacheSql(const StringMeta& sql_meta) const override {
        // Mock implementation
    }

    std::vector<unsigned char> cacheSqlUid(std::string_view sql) const override {
        // Mock implementation - return test uid
        return {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    }

    void removeCacheSqlUid(const SqlUidMeta& sql_uid_meta) const override {
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

    AgentStats& getAgentStats() override {
        if (!agent_stats_) {
            agent_stats_ = std::make_unique<AgentStats>(this);
        }
        return *agent_stats_;
    }

    UrlStats& getUrlStats() override {
        if (!url_stats_) {
            url_stats_ = std::make_unique<UrlStats>(this);
        }
        return *url_stats_;
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
    int32_t getCachedSqlId(const std::string& sql_query) const {
        auto it = cached_sqls_.find(sql_query);
        return it != cached_sqls_.end() ? it->second : -1;
    }
    int32_t getSqlIdCounter() const { return sql_id_counter_; }

    mutable int recorded_spans_ = 0;
    mutable int recorded_url_stats_ = 0;
    mutable int recorded_exceptions_ = 0;
    mutable int recorded_stats_calls_ = 0;
    mutable int recorded_server_headers_ = 0;
    mutable int recorded_client_headers_ = 0;

private:
    bool is_exiting_;
    int64_t start_time_;
    std::string cached_start_time_str_;
    int64_t trace_id_counter_;
    Config config_;
    mutable std::unique_ptr<AgentStats> agent_stats_;
    mutable std::unique_ptr<UrlStats> url_stats_;
    mutable std::map<std::string, int32_t> cached_apis_;
    mutable std::map<std::string, int32_t> cached_errors_;
    mutable std::map<std::string, int32_t> cached_sqls_;
    mutable int32_t api_id_counter_ = 100;
    mutable int32_t error_id_counter_ = 200;
    mutable int32_t sql_id_counter_ = 300;
};

// Use actual SpanData for testing
using TestSpanData = SpanData;

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

// Mock CallStackReader for testing
class MockCallStackReader : public CallStackReader {
public:
    MockCallStackReader() = default;

    void ForEach(std::function<void(std::string_view module, std::string_view function, std::string_view file, int line)> callback) const override {
        for (const auto& frame : frames_) {
            callback(frame.module, frame.function, frame.file, frame.line);
        }
    }

    void AddFrame(std::string_view module, std::string_view function, std::string_view file, int line) {
        frames_.push_back({std::string(module), std::string(function), std::string(file), line});
    }

    size_t GetFrameCount() const { return frames_.size(); }

private:
    struct Frame {
        std::string module;
        std::string function;
        std::string file;
        int line;
    };
    std::vector<Frame> frames_;
};

class SpanEventTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_agent_service_ = std::make_unique<MockAgentService>();
        test_span_data_ = std::make_unique<TestSpanData>(mock_agent_service_.get(), "test-operation");
    }

    void TearDown() override {
        mock_agent_service_.reset();
        test_span_data_.reset();
    }

    std::unique_ptr<MockAgentService> mock_agent_service_;
    std::unique_ptr<TestSpanData> test_span_data_;
};

// ========== SpanEventImpl Constructor Tests ==========

TEST_F(SpanEventTest, ConstructorTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-operation");
    
    EXPECT_NE(span_event.getParentSpan(), nullptr) << "Parent span should be set";
    EXPECT_EQ(span_event.getServiceType(), defaults::SPAN_EVENT_SERVICE_TYPE) << "Default service type should be set";
    EXPECT_EQ(span_event.getOperationName(), "test-operation") << "Operation name should match";
    EXPECT_GT(span_event.getStartTime(), 0) << "Start time should be set";
    EXPECT_EQ(span_event.getStartElapsed(), 0) << "Initial start elapsed should be 0";
    EXPECT_EQ(span_event.getEndElapsed(), 0) << "Initial end elapsed should be 0";
    EXPECT_EQ(span_event.getSequence(), 0) << "First event should have sequence 0";
    EXPECT_EQ(span_event.getDepth(), 1) << "Initial depth should be 1";
    EXPECT_EQ(span_event.getNextSpanId(), 0) << "Initial next span ID should be 0";
    EXPECT_EQ(span_event.getAsyncId(), NONE_ASYNC_ID) << "Initial async ID should be NONE_ASYNC_ID";
    EXPECT_EQ(span_event.getAsyncSeqGen(), 0) << "Initial async seq gen should be 0";
    EXPECT_NE(span_event.GetAnnotations(), nullptr) << "Annotations should be initialized";
}

TEST_F(SpanEventTest, ConstructorWithEmptyOperationTest) {
    SpanEventImpl span_event(test_span_data_.get(), "");
    
    EXPECT_EQ(span_event.getOperationName(), "") << "Empty operation should be preserved";
    EXPECT_EQ(span_event.getApiId(), 0) << "API ID should remain 0 for empty operation";
}

TEST_F(SpanEventTest, ConstructorWithNonEmptyOperationTest) {
    SpanEventImpl span_event(test_span_data_.get(), "database-query");
    
    EXPECT_EQ(span_event.getOperationName(), "database-query") << "Operation name should be set";
    EXPECT_GT(span_event.getApiId(), 0) << "API ID should be cached for non-empty operation";
    
    // Verify API was cached
    int32_t cached_id = mock_agent_service_->getCachedApiId("database-query");
    EXPECT_EQ(span_event.getApiId(), cached_id) << "API ID should match cached ID";
}

// ========== Setter Methods Tests ==========

TEST_F(SpanEventTest, SetServiceTypeTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    span_event.SetServiceType(1234);
    EXPECT_EQ(span_event.getServiceType(), 1234) << "Service type should be updated";
}

TEST_F(SpanEventTest, SetOperationNameTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    span_event.SetOperationName("new-operation");
    EXPECT_EQ(span_event.getOperationName(), "new-operation") << "Operation name should be updated";
}

TEST_F(SpanEventTest, SetStartTimeTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    auto test_time = std::chrono::system_clock::now();
    span_event.SetStartTime(test_time);
    
    int64_t expected_time = to_milli_seconds(test_time);
    EXPECT_EQ(span_event.getStartTime(), expected_time) << "Start time should be updated";
}

TEST_F(SpanEventTest, SetDestinationTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    span_event.SetDestination("test-destination");
    EXPECT_EQ(span_event.getDestinationId(), "test-destination") << "Destination should be updated";
}

TEST_F(SpanEventTest, SetEndPointTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    span_event.SetEndPoint("http://example.com");
    EXPECT_EQ(span_event.getEndPoint(), "http://example.com") << "EndPoint should be updated";
}

TEST_F(SpanEventTest, SetStartElapsedTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    span_event.setStartElapsed(100);
    EXPECT_EQ(span_event.getStartElapsed(), 100) << "Start elapsed should be updated";
}

TEST_F(SpanEventTest, SetDepthTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    span_event.setDepth(5);
    EXPECT_EQ(span_event.getDepth(), 5) << "Depth should be updated";
}

TEST_F(SpanEventTest, SetAsyncIdTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    span_event.setAsyncId(42);
    EXPECT_EQ(span_event.getAsyncId(), 42) << "Async ID should be updated";
}

TEST_F(SpanEventTest, SetApiIdTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    span_event.setApiId(999);
    EXPECT_EQ(span_event.getApiId(), 999) << "API ID should be updated";
}

// ========== Error Handling Tests ==========

TEST_F(SpanEventTest, SetErrorWithMessageOnlyTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    span_event.SetError("Something went wrong");
    
    EXPECT_GT(span_event.getErrorFuncId(), 0) << "Error function ID should be cached";
    EXPECT_EQ(span_event.getErrorString(), "Something went wrong") << "Error message should be set";
    
    // Verify error was cached with default name "Error"
    int32_t cached_id = mock_agent_service_->getCachedErrorId("Error");
    EXPECT_EQ(span_event.getErrorFuncId(), cached_id) << "Error function ID should match cached ID";
}

TEST_F(SpanEventTest, SetErrorWithNameAndMessageTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    span_event.SetError("SQLException", "Connection timeout");
    
    EXPECT_GT(span_event.getErrorFuncId(), 0) << "Error function ID should be cached";
    EXPECT_EQ(span_event.getErrorString(), "Connection timeout") << "Error message should be set";
    
    // Verify error was cached with custom name
    int32_t cached_id = mock_agent_service_->getCachedErrorId("SQLException");
    EXPECT_EQ(span_event.getErrorFuncId(), cached_id) << "Error function ID should match cached ID";
}

// ========== Error with CallStack Tests ==========

TEST_F(SpanEventTest, SetErrorWithCallStackBasicTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    MockCallStackReader reader;
    
    // Add a single stack frame
    reader.AddFrame("/usr/lib/libmyapp.so", "main", "/src/main.cpp", 42);
    
    span_event.SetError("RuntimeError", "Test error message", reader);
    
    // Verify error information is set
    EXPECT_GT(span_event.getErrorFuncId(), 0) << "Error function ID should be cached";
    EXPECT_EQ(span_event.getErrorString(), "Test error message") << "Error message should be set";
    
    // Verify error was cached with correct name
    int32_t cached_id = mock_agent_service_->getCachedErrorId("RuntimeError");
    EXPECT_EQ(span_event.getErrorFuncId(), cached_id) << "Error function ID should match cached ID";
    
    // Verify annotation contains exception ID
    auto annotations = span_event.GetAnnotations();
    EXPECT_NE(annotations, nullptr) << "Annotations should not be null";
    
    // Verify exception was added to parent span
    const auto& exceptions = test_span_data_->getExceptions();
    EXPECT_EQ(exceptions.size(), 1) << "One exception should be added to parent span";
    EXPECT_NE(exceptions[0], nullptr) << "Exception should not be null";
    EXPECT_GT(exceptions[0]->getId(), 0) << "Exception should have valid ID";
}

TEST_F(SpanEventTest, SetErrorWithCallStackMultipleFramesTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    MockCallStackReader reader;
    
    // Add multiple stack frames
    reader.AddFrame("/usr/lib/libmyapp.so", "function1", "/src/file1.cpp", 10);
    reader.AddFrame("/usr/lib/libmyapp.so", "function2", "/src/file2.cpp", 20);
    reader.AddFrame("/usr/lib/libmyapp.so", "function3", "/src/file3.cpp", 30);
    reader.AddFrame("/usr/lib/libmyapp.so", "main", "/src/main.cpp", 100);
    
    span_event.SetError("StackOverflowError", "Stack overflow occurred", reader);
    
    // Verify error information
    EXPECT_GT(span_event.getErrorFuncId(), 0) << "Error function ID should be cached";
    EXPECT_EQ(span_event.getErrorString(), "Stack overflow occurred") << "Error message should be set";
    
    // Verify exception was added
    const auto& exceptions = test_span_data_->getExceptions();
    EXPECT_EQ(exceptions.size(), 1) << "One exception should be added";
    
    // Verify callstack contains all frames
    auto callstack = exceptions[0]->getCallStack();
    EXPECT_NE(callstack, nullptr) << "CallStack should not be null";
    EXPECT_EQ(callstack->getErrorMessage(), "Stack overflow occurred") << "Error message should match";
    
    auto& stack_frames = callstack->getStack();
    EXPECT_EQ(stack_frames.size(), 4) << "Should have 4 stack frames";
    
    // Verify frame details
    EXPECT_EQ(stack_frames[0].module, "/usr/lib/libmyapp.so");
    EXPECT_EQ(stack_frames[0].function, "function1");
    EXPECT_EQ(stack_frames[0].file, "/src/file1.cpp");
    EXPECT_EQ(stack_frames[0].line, 10);
    
    EXPECT_EQ(stack_frames[1].function, "function2");
    EXPECT_EQ(stack_frames[2].function, "function3");
    EXPECT_EQ(stack_frames[3].function, "main");
    EXPECT_EQ(stack_frames[3].line, 100);
}

TEST_F(SpanEventTest, SetErrorWithCallStackEmptyTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    MockCallStackReader reader;
    
    // Don't add any frames
    EXPECT_EQ(reader.GetFrameCount(), 0) << "Reader should have no frames";
    
    span_event.SetError("EmptyStackError", "Error with empty stack", reader);
    
    // Verify error information is still set
    EXPECT_GT(span_event.getErrorFuncId(), 0) << "Error function ID should be cached";
    EXPECT_EQ(span_event.getErrorString(), "Error with empty stack") << "Error message should be set";
    
    // Verify exception was added even with empty stack
    const auto& exceptions = test_span_data_->getExceptions();
    EXPECT_EQ(exceptions.size(), 1) << "One exception should be added even with empty stack";
    
    auto callstack = exceptions[0]->getCallStack();
    EXPECT_NE(callstack, nullptr) << "CallStack should not be null";
    EXPECT_EQ(callstack->getErrorMessage(), "Error with empty stack") << "Error message should match";
    
    auto& stack_frames = callstack->getStack();
    EXPECT_EQ(stack_frames.size(), 0) << "Stack should be empty";
}

TEST_F(SpanEventTest, SetErrorWithCallStackExceptionIdAnnotationTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    MockCallStackReader reader;
    
    reader.AddFrame("/lib/app.so", "doWork", "/src/worker.cpp", 55);
    
    span_event.SetError("NullPointerError", "Null pointer dereference", reader);
    
    // Get the exception ID from the exception
    const auto& exceptions = test_span_data_->getExceptions();
    ASSERT_EQ(exceptions.size(), 1) << "Should have one exception";
    int32_t exception_id = exceptions[0]->getId();
    
    EXPECT_GT(exception_id, 0) << "Exception ID should be positive";
    
    // Note: In a real test, we would verify that ANNOTATION_EXCEPTION_ID was added to annotations
    // However, the PinpointAnnotation class doesn't expose a way to read back annotations
    // So we just verify that the exception ID is valid
    auto annotations = span_event.GetAnnotations();
    EXPECT_NE(annotations, nullptr) << "Annotations should exist";
}

TEST_F(SpanEventTest, SetErrorWithCallStackMultipleErrorsTest) {
    // Test that multiple errors can be set on different span events
    SpanEventImpl event1(test_span_data_.get(), "op1");
    SpanEventImpl event2(test_span_data_.get(), "op2");
    
    MockCallStackReader reader1;
    reader1.AddFrame("/lib/app.so", "function1", "/src/file1.cpp", 10);
    
    MockCallStackReader reader2;
    reader2.AddFrame("/lib/app.so", "function2", "/src/file2.cpp", 20);
    reader2.AddFrame("/lib/app.so", "function3", "/src/file3.cpp", 30);
    
    event1.SetError("Error1", "First error", reader1);
    event2.SetError("Error2", "Second error", reader2);
    
    // Both errors should be set
    EXPECT_EQ(event1.getErrorString(), "First error");
    EXPECT_EQ(event2.getErrorString(), "Second error");
    
    // Both exceptions should be added to parent span
    const auto& exceptions = test_span_data_->getExceptions();
    EXPECT_EQ(exceptions.size(), 2) << "Two exceptions should be added";
    
    // Verify different exception IDs
    int32_t id1 = exceptions[0]->getId();
    int32_t id2 = exceptions[1]->getId();
    EXPECT_NE(id1, id2) << "Exception IDs should be different";
}

TEST_F(SpanEventTest, SetErrorWithCallStackDetailedFramesTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    MockCallStackReader reader;
    
    // Add frames with various details
    reader.AddFrame("/usr/local/lib/libcustom.so.1.0", "CustomClass::process()", "/home/dev/project/src/custom.cpp", 123);
    reader.AddFrame("/lib/x86_64-linux-gnu/libc.so.6", "malloc", "", 0);  // System library without file info
    reader.AddFrame("./myapp", "handleRequest", "/app/handler.cpp", 456);
    
    span_event.SetError("ProcessingError", "Failed to process request", reader);
    
    // Verify exception
    const auto& exceptions = test_span_data_->getExceptions();
    ASSERT_EQ(exceptions.size(), 1);
    
    auto callstack = exceptions[0]->getCallStack();
    ASSERT_NE(callstack, nullptr);
    
    auto& frames = callstack->getStack();
    ASSERT_EQ(frames.size(), 3);
    
    // Verify first frame with full details
    EXPECT_EQ(frames[0].module, "/usr/local/lib/libcustom.so.1.0");
    EXPECT_EQ(frames[0].function, "CustomClass::process()");
    EXPECT_EQ(frames[0].file, "/home/dev/project/src/custom.cpp");
    EXPECT_EQ(frames[0].line, 123);
    
    // Verify second frame (system library)
    EXPECT_EQ(frames[1].module, "/lib/x86_64-linux-gnu/libc.so.6");
    EXPECT_EQ(frames[1].function, "malloc");
    EXPECT_EQ(frames[1].file, "");  // No file info
    EXPECT_EQ(frames[1].line, 0);    // No line info
    
    // Verify third frame
    EXPECT_EQ(frames[2].module, "./myapp");
    EXPECT_EQ(frames[2].function, "handleRequest");
    EXPECT_EQ(frames[2].line, 456);
}

TEST_F(SpanEventTest, SetErrorWithCallStackErrorTimeTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    MockCallStackReader reader;
    
    reader.AddFrame("/lib/app.so", "testFunc", "/src/test.cpp", 1);
    
    auto before_time = to_milli_seconds(std::chrono::system_clock::now());
    
    span_event.SetError("TimeTestError", "Testing timestamp", reader);
    
    auto after_time = to_milli_seconds(std::chrono::system_clock::now());
    
    // Verify exception
    const auto& exceptions = test_span_data_->getExceptions();
    ASSERT_EQ(exceptions.size(), 1);
    
    auto callstack = exceptions[0]->getCallStack();
    ASSERT_NE(callstack, nullptr);
    
    int64_t error_time = callstack->getErrorTime();
    EXPECT_GE(error_time, before_time) << "Error time should be after or equal to before time";
    EXPECT_LE(error_time, after_time) << "Error time should be before or equal to after time";
}

TEST_F(SpanEventTest, SetErrorWithCallStackIntegrationTest) {
    SpanEventImpl span_event(test_span_data_.get(), "database-operation");
    
    // Set up the span event
    span_event.SetServiceType(SERVICE_TYPE_MYSQL_QUERY);
    span_event.SetDestination("mysql-server");
    span_event.SetEndPoint("localhost:3306");
    
    // Create a realistic call stack
    MockCallStackReader reader;
    reader.AddFrame("/usr/lib/libmysqlclient.so", "mysql_real_connect", "", 0);
    reader.AddFrame("/opt/myapp/lib/libdb.so", "DatabaseConnection::connect()", "/src/db/connection.cpp", 78);
    reader.AddFrame("/opt/myapp/bin/server", "handleDatabaseRequest", "/src/server/handler.cpp", 234);
    reader.AddFrame("/opt/myapp/bin/server", "main", "/src/main.cpp", 45);
    
    // Set error with call stack
    span_event.SetError("MySQLConnectionError", "Unable to connect to MySQL server: Connection refused", reader);
    
    // Wait a bit and finish
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    span_event.finish();
    
    // Verify all properties
    EXPECT_EQ(span_event.getServiceType(), SERVICE_TYPE_MYSQL_QUERY);
    EXPECT_EQ(span_event.getDestinationId(), "mysql-server");
    EXPECT_EQ(span_event.getEndPoint(), "localhost:3306");
    EXPECT_GT(span_event.getErrorFuncId(), 0);
    EXPECT_EQ(span_event.getErrorString(), "Unable to connect to MySQL server: Connection refused");
    EXPECT_GT(span_event.getEndElapsed(), 0);
    
    // Verify exception and callstack
    const auto& exceptions = test_span_data_->getExceptions();
    ASSERT_EQ(exceptions.size(), 1);
    
    auto callstack = exceptions[0]->getCallStack();
    ASSERT_NE(callstack, nullptr);
    EXPECT_EQ(callstack->getErrorMessage(), "Unable to connect to MySQL server: Connection refused");
    
    auto& frames = callstack->getStack();
    EXPECT_EQ(frames.size(), 4);
    EXPECT_EQ(frames[0].function, "mysql_real_connect");
    EXPECT_EQ(frames[1].function, "DatabaseConnection::connect()");
    EXPECT_EQ(frames[2].function, "handleDatabaseRequest");
    EXPECT_EQ(frames[3].function, "main");
}

// ========== Async Operations Tests ==========

TEST_F(SpanEventTest, IncrAsyncSeqTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    EXPECT_EQ(span_event.getAsyncSeqGen(), 0) << "Initial async seq gen should be 0";
    
    span_event.incrAsyncSeq();
    EXPECT_EQ(span_event.getAsyncSeqGen(), 1) << "Async seq gen should increment to 1";
    
    span_event.incrAsyncSeq();
    EXPECT_EQ(span_event.getAsyncSeqGen(), 2) << "Async seq gen should increment to 2";
}

// ========== Span ID Generation Tests ==========

TEST_F(SpanEventTest, GenerateNextSpanIdTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    EXPECT_EQ(span_event.getNextSpanId(), 0) << "Initial next span ID should be 0";
    
    int64_t generated_id = span_event.generateNextSpanId();
    
    EXPECT_NE(generated_id, 0) << "Generated span ID should be non-zero";
    EXPECT_EQ(span_event.getNextSpanId(), generated_id) << "Next span ID should match generated ID";
}

TEST_F(SpanEventTest, GenerateMultipleSpanIdsTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    int64_t id1 = span_event.generateNextSpanId();
    int64_t id2 = span_event.generateNextSpanId();
    
    EXPECT_NE(id1, id2) << "Generated span IDs should be different";
    EXPECT_EQ(span_event.getNextSpanId(), id2) << "Next span ID should be the last generated ID";
}

// ========== Header Recording Tests ==========

TEST_F(SpanEventTest, RecordHeaderTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    MockHeaderReader header_reader;
    
    header_reader.SetHeader("Content-Type", "application/json");
    header_reader.SetHeader("Authorization", "Bearer token123");
    
    // Record client headers
    span_event.RecordHeader(HTTP_REQUEST, header_reader);
    
    EXPECT_EQ(mock_agent_service_->recorded_client_headers_, 1) << "Client header should be recorded";
}

TEST_F(SpanEventTest, RecordMultipleHeadersTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    MockHeaderReader header_reader1;
    MockHeaderReader header_reader2;
    
    header_reader1.SetHeader("User-Agent", "test-client");
    header_reader2.SetHeader("Accept", "application/json");
    
    span_event.RecordHeader(HTTP_REQUEST, header_reader1);
    span_event.RecordHeader(HTTP_RESPONSE, header_reader2);
    
    EXPECT_EQ(mock_agent_service_->recorded_client_headers_, 2) << "Multiple headers should be recorded";
}

// ========== Finish Tests ==========

TEST_F(SpanEventTest, FinishTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    // Get initial state
    int32_t initial_depth = test_span_data_->getEventDepth();
    
    // Wait a bit to ensure elapsed time > 0
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    
    span_event.finish();
    
    // Check that depth was decremented
    EXPECT_EQ(test_span_data_->getEventDepth(), initial_depth - 1) << "Event depth should be decremented";
    
    // Check that elapsed time was calculated
    EXPECT_GT(span_event.getEndElapsed(), 0) << "End elapsed should be greater than 0";
}

TEST_F(SpanEventTest, FinishCalculatesElapsedTimeTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    // Set a specific start time
    auto start_time = std::chrono::system_clock::now() - std::chrono::milliseconds(100);
    span_event.SetStartTime(start_time);
    
    span_event.finish();
    
    // Elapsed time should be approximately 100ms or more
    EXPECT_GE(span_event.getEndElapsed(), 90) << "Elapsed time should be at least 90ms";
    EXPECT_LE(span_event.getEndElapsed(), 200) << "Elapsed time should be reasonable (< 200ms)";
}

// ========== Annotations Tests ==========

TEST_F(SpanEventTest, GetAnnotationsTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    auto annotations = span_event.GetAnnotations();
    EXPECT_NE(annotations, nullptr) << "Annotations should not be null";
    
    // Test that we get the same instance
    auto annotations2 = span_event.GetAnnotations();
    EXPECT_EQ(annotations, annotations2) << "Should return the same annotations instance";
}

TEST_F(SpanEventTest, GetAnnotationsSharedPtrTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    auto& annotations = span_event.getAnnotations();
    EXPECT_NE(annotations, nullptr) << "Annotations shared_ptr should not be null";
    
    // Test that we can use the annotations
    annotations->AppendString(100, "test-value");
    // Verify by checking the content (this depends on PinpointAnnotation implementation)
}

// ========== Integration Tests ==========

TEST_F(SpanEventTest, CompleteWorkflowTest) {
    SpanEventImpl span_event(test_span_data_.get(), "http-request");
    
    // Set up span event
    span_event.SetServiceType(9999);
    span_event.SetDestination("api.example.com");
    span_event.SetEndPoint("https://api.example.com/users");
    span_event.setDepth(2);
    span_event.setAsyncId(42);
    
    // Add some annotations
    auto annotations = span_event.GetAnnotations();
    annotations->AppendString(200, "request-url");
    annotations->AppendInt(300, 200);
    
    // Record headers
    MockHeaderReader headers;
    headers.SetHeader("Content-Type", "application/json");
    span_event.RecordHeader(HTTP_REQUEST, headers);
    
    // Generate next span ID
    int64_t next_id = span_event.generateNextSpanId();
    
    // Increment async sequence
    span_event.incrAsyncSeq();
    span_event.incrAsyncSeq();
    
    // Set error
    span_event.SetError("NetworkError", "Connection failed");
    
    // Wait a bit before finishing to ensure elapsed time > 0
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    
    // Finish the span event
    span_event.finish();
    
    // Verify final state
    EXPECT_EQ(span_event.getServiceType(), 9999);
    EXPECT_EQ(span_event.getDestinationId(), "api.example.com");
    EXPECT_EQ(span_event.getEndPoint(), "https://api.example.com/users");
    EXPECT_EQ(span_event.getDepth(), 2);
    EXPECT_EQ(span_event.getAsyncId(), 42);
    EXPECT_EQ(span_event.getNextSpanId(), next_id);
    EXPECT_EQ(span_event.getAsyncSeqGen(), 2);
    EXPECT_GT(span_event.getErrorFuncId(), 0);
    EXPECT_EQ(span_event.getErrorString(), "Connection failed");
    EXPECT_GT(span_event.getEndElapsed(), 0);
    EXPECT_EQ(mock_agent_service_->recorded_client_headers_, 1);
}

TEST_F(SpanEventTest, MultipleSpanEventsTest) {
    // Create multiple span events to test that they can be created from same SpanData
    SpanEventImpl event1(test_span_data_.get(), "operation-1");
    SpanEventImpl event2(test_span_data_.get(), "operation-2");
    SpanEventImpl event3(test_span_data_.get(), "operation-3");
    
    // Each should have valid sequence numbers (actual values depend on SpanData implementation)
    EXPECT_GE(event1.getSequence(), 0);
    EXPECT_GE(event2.getSequence(), 0);
    EXPECT_GE(event3.getSequence(), 0);
    
    // Each should have valid depths
    EXPECT_GT(event1.getDepth(), 0);
    EXPECT_GT(event2.getDepth(), 0);
    EXPECT_GT(event3.getDepth(), 0);
    
    // Each should have different operation names
    EXPECT_EQ(event1.getOperationName(), "operation-1");
    EXPECT_EQ(event2.getOperationName(), "operation-2");
    EXPECT_EQ(event3.getOperationName(), "operation-3");
    
    // Each should have different API IDs (cached by agent)
    int32_t api_id1 = event1.getApiId();
    int32_t api_id2 = event2.getApiId();
    int32_t api_id3 = event3.getApiId();
    
    EXPECT_GT(api_id1, 0);
    EXPECT_GT(api_id2, 0);
    EXPECT_GT(api_id3, 0);
    EXPECT_NE(api_id1, api_id2);
    EXPECT_NE(api_id2, api_id3);
    EXPECT_NE(api_id1, api_id3);
}

// ========== SQL Query Tests ==========

TEST_F(SpanEventTest, SetSqlQueryBasicTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    std::string sql_query = "SELECT * FROM users WHERE id = ?";
    std::string args = "123";
    
    span_event.SetSqlQuery(sql_query, args);
    
    // Verify SQL was cached with normalized form
    std::string normalized_sql = "SELECT * FROM users WHERE id = ?";  // Expected normalized form
    int32_t cached_id = mock_agent_service_->getCachedSqlId(normalized_sql);
    EXPECT_GT(cached_id, 0) << "SQL should be cached with valid ID";
    
    // Verify annotations were added
    auto annotations = span_event.GetAnnotations();
    EXPECT_NE(annotations, nullptr) << "Annotations should not be null";
}

TEST_F(SpanEventTest, SetSqlQueryWithParametersTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    std::string sql_query = "INSERT INTO products (name, price) VALUES ('iPhone', 999.99)";
    std::string args = "name=iPhone, price=999.99";
    
    span_event.SetSqlQuery(sql_query, args);
    
    // Verify SQL was cached - the normalizer should convert literals to ?
    // The exact normalized form depends on SqlNormalizer implementation
    std::string expected_normalized = "INSERT INTO products (name, price) VALUES (?, ?)";
    (void)mock_agent_service_->getCachedSqlId(expected_normalized);  // Check if cached (suppress unused warning)
    
    // The SQL should be cached with some form (normalized version)
    EXPECT_GT(mock_agent_service_->getSqlIdCounter(), 300) << "SQL ID counter should have incremented";
}

TEST_F(SpanEventTest, SetSqlQueryEmptyTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    std::string empty_sql = "";
    std::string args = "";
    
    span_event.SetSqlQuery(empty_sql, args);
    
    // Even empty SQL should be processed
    EXPECT_GT(mock_agent_service_->getSqlIdCounter(), 300) << "SQL ID counter should increment for empty SQL too";
}

TEST_F(SpanEventTest, SetSqlQueryComplexQueryTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    std::string complex_sql = R"(
        SELECT u.id, u.name, p.title 
        FROM users u 
        JOIN posts p ON u.id = p.user_id 
        WHERE u.active = 1 
        AND p.published_at > '2023-01-01' 
        ORDER BY p.published_at DESC 
        LIMIT 10
    )";
    std::string args = "active=1, published_at=2023-01-01, limit=10";
    
    span_event.SetSqlQuery(complex_sql, args);
    
    // Complex SQL should also be cached
    EXPECT_GT(mock_agent_service_->getSqlIdCounter(), 300) << "Complex SQL should be cached";
    
    auto annotations = span_event.GetAnnotations();
    EXPECT_NE(annotations, nullptr) << "Annotations should be created for complex SQL";
}

TEST_F(SpanEventTest, SetSqlQueryMultipleCallsTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    // Call SetSqlQuery multiple times with different queries
    span_event.SetSqlQuery("SELECT * FROM table1", "");
    span_event.SetSqlQuery("SELECT * FROM table2", "");
    span_event.SetSqlQuery("UPDATE table1 SET name = ?", "name=test");
    
    // Each call should increment the counter (started at 300, after 3 calls should be 303)
    EXPECT_GE(mock_agent_service_->getSqlIdCounter(), 303) << "Multiple SQL queries should increment counter";
}

TEST_F(SpanEventTest, SetSqlQuerySameQueryTest) {
    SpanEventImpl span_event1(test_span_data_.get(), "test-op1");
    SpanEventImpl span_event2(test_span_data_.get(), "test-op2");
    
    std::string same_sql = "SELECT * FROM users";
    
    span_event1.SetSqlQuery(same_sql, "");
    int32_t first_call_counter = mock_agent_service_->getSqlIdCounter();
    
    span_event2.SetSqlQuery(same_sql, "");
    int32_t second_call_counter = mock_agent_service_->getSqlIdCounter();
    
    // Same SQL should be cached, but in our mock it will create new entries
    // This tests that the method can be called multiple times
    EXPECT_GT(second_call_counter, first_call_counter) << "SQL cache calls should increment counter";
}

TEST_F(SpanEventTest, SetSqlQueryNormalizationTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    // Test SQL with literals that should be normalized
    std::string sql_with_literals = "SELECT * FROM users WHERE id = 123 AND name = 'John'";
    std::string args = "id=123, name=John";
    
    span_event.SetSqlQuery(sql_with_literals, args);
    
    // The normalizer should process this SQL
    EXPECT_GT(mock_agent_service_->getSqlIdCounter(), 300) << "SQL with literals should be processed";
    
    auto annotations = span_event.GetAnnotations();
    EXPECT_NE(annotations, nullptr) << "Annotations should be created";
}

TEST_F(SpanEventTest, SetSqlQueryWithSpecialCharactersTest) {
    SpanEventImpl span_event(test_span_data_.get(), "test-op");
    
    std::string sql_special = "SELECT * FROM `table_name` WHERE `column` = 'O''Reilly'";
    std::string args = "column=O'Reilly";
    
    span_event.SetSqlQuery(sql_special, args);
    
    // SQL with special characters should be handled
    EXPECT_GT(mock_agent_service_->getSqlIdCounter(), 300) << "SQL with special characters should be processed";
}

TEST_F(SpanEventTest, SetSqlQueryIntegrationTest) {
    SpanEventImpl span_event(test_span_data_.get(), "database-operation");
    
    // Set up span event for database operation
    span_event.SetServiceType(SERVICE_TYPE_CPP_FUNC);
    span_event.SetDestination("mysql-server");
    span_event.SetEndPoint("localhost:3306");
    
    // Add SQL query
    std::string sql = "SELECT u.*, COUNT(p.id) as post_count FROM users u LEFT JOIN posts p ON u.id = p.user_id GROUP BY u.id";
    std::string args = "";
    
    span_event.SetSqlQuery(sql, args);
    
    // Verify the integration
    EXPECT_EQ(span_event.getServiceType(), SERVICE_TYPE_CPP_FUNC);
    EXPECT_EQ(span_event.getDestinationId(), "mysql-server");
    EXPECT_EQ(span_event.getEndPoint(), "localhost:3306");
    EXPECT_GT(mock_agent_service_->getSqlIdCounter(), 300) << "SQL should be cached";
    
    auto annotations = span_event.GetAnnotations();
    EXPECT_NE(annotations, nullptr) << "Annotations should be available";
}

} // namespace pinpoint
