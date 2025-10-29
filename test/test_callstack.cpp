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

#include "../src/callstack.h"
#include <gtest/gtest.h>
#include <string>
#include <memory>
#include <thread>
#include <chrono>

namespace pinpoint {

class CallStackTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Called before each test
    }

    void TearDown() override {
        // Called after each test
    }
};

// ========== StackFrame Tests ==========

// Test StackFrame structure creation
TEST_F(CallStackTest, StackFrameCreationTest) {
    StackFrame frame;
    frame.module = "test_module";
    frame.function = "test_function";
    frame.file = "test_file.cpp";
    frame.line = 42;
    
    EXPECT_EQ(frame.module, "test_module") << "Module name should match";
    EXPECT_EQ(frame.function, "test_function") << "Function name should match";
    EXPECT_EQ(frame.file, "test_file.cpp") << "File name should match";
    EXPECT_EQ(frame.line, 42) << "Line number should match";
}

// ========== CallStack Tests ==========

// Test CallStack creation with error message
TEST_F(CallStackTest, CallStackCreationTest) {
    std::string error_msg = "Test error message";
    CallStack callstack(error_msg);
    
    EXPECT_EQ(callstack.getErrorMessage(), error_msg) << "Error message should match";
    EXPECT_GT(callstack.getErrorTime(), 0) << "Error time should be greater than 0";
    EXPECT_TRUE(callstack.getStack().empty()) << "Stack should be empty on creation";
}

// Test CallStack with empty error message
TEST_F(CallStackTest, CallStackEmptyErrorMessageTest) {
    std::string error_msg = "";
    CallStack callstack(error_msg);
    
    EXPECT_EQ(callstack.getErrorMessage(), "") << "Empty error message should be preserved";
    EXPECT_GT(callstack.getErrorTime(), 0) << "Error time should be greater than 0 even with empty message";
}

// Test CallStack with special characters in error message
TEST_F(CallStackTest, CallStackSpecialCharsErrorMessageTest) {
    std::string error_msg = "Error: !@#$%^&*()_+[]{}|;':\",./<>?\\`~\n\t\rException occurred";
    CallStack callstack(error_msg);
    
    EXPECT_EQ(callstack.getErrorMessage(), error_msg) << "Error message with special chars should match";
}

// Test CallStack with Unicode error message
TEST_F(CallStackTest, CallStackUnicodeErrorMessageTest) {
    std::string error_msg = "오류 발생: 예외 처리 필요 (에러 코드: 500)";
    CallStack callstack(error_msg);
    
    EXPECT_EQ(callstack.getErrorMessage(), error_msg) << "Unicode error message should match";
}

// Test CallStack push single frame
TEST_F(CallStackTest, CallStackPushSingleFrameTest) {
    CallStack callstack("Test error");
    
    callstack.push("module1", "function1", "file1.cpp", 10);
    
    auto& stack = callstack.getStack();
    EXPECT_EQ(stack.size(), 1) << "Stack should have 1 frame";
    EXPECT_EQ(stack[0].module, "module1") << "Module name should match";
    EXPECT_EQ(stack[0].function, "function1") << "Function name should match";
    EXPECT_EQ(stack[0].file, "file1.cpp") << "File name should match";
    EXPECT_EQ(stack[0].line, 10) << "Line number should match";
}

// Test CallStack push multiple frames
TEST_F(CallStackTest, CallStackPushMultipleFramesTest) {
    CallStack callstack("Test error");
    
    callstack.push("module1", "function1", "file1.cpp", 10);
    callstack.push("module2", "function2", "file2.cpp", 20);
    callstack.push("module3", "function3", "file3.cpp", 30);
    
    auto& stack = callstack.getStack();
    EXPECT_EQ(stack.size(), 3) << "Stack should have 3 frames";
    
    // Check first frame
    EXPECT_EQ(stack[0].module, "module1") << "First frame module should match";
    EXPECT_EQ(stack[0].function, "function1") << "First frame function should match";
    EXPECT_EQ(stack[0].file, "file1.cpp") << "First frame file should match";
    EXPECT_EQ(stack[0].line, 10) << "First frame line should match";
    
    // Check second frame
    EXPECT_EQ(stack[1].module, "module2") << "Second frame module should match";
    EXPECT_EQ(stack[1].function, "function2") << "Second frame function should match";
    EXPECT_EQ(stack[1].file, "file2.cpp") << "Second frame file should match";
    EXPECT_EQ(stack[1].line, 20) << "Second frame line should match";
    
    // Check third frame
    EXPECT_EQ(stack[2].module, "module3") << "Third frame module should match";
    EXPECT_EQ(stack[2].function, "function3") << "Third frame function should match";
    EXPECT_EQ(stack[2].file, "file3.cpp") << "Third frame file should match";
    EXPECT_EQ(stack[2].line, 30) << "Third frame line should match";
}

// Test CallStack push with empty strings
TEST_F(CallStackTest, CallStackPushEmptyStringsTest) {
    CallStack callstack("Test error");
    
    callstack.push("", "", "", 0);
    
    auto& stack = callstack.getStack();
    EXPECT_EQ(stack.size(), 1) << "Stack should have 1 frame";
    EXPECT_EQ(stack[0].module, "") << "Empty module should be preserved";
    EXPECT_EQ(stack[0].function, "") << "Empty function should be preserved";
    EXPECT_EQ(stack[0].file, "") << "Empty file should be preserved";
    EXPECT_EQ(stack[0].line, 0) << "Line number should be 0";
}

// Test CallStack push with negative line number
TEST_F(CallStackTest, CallStackPushNegativeLineTest) {
    CallStack callstack("Test error");
    
    callstack.push("module", "function", "file.cpp", -1);
    
    auto& stack = callstack.getStack();
    EXPECT_EQ(stack.size(), 1) << "Stack should have 1 frame";
    EXPECT_EQ(stack[0].line, -1) << "Negative line number should be preserved";
}

// Test CallStack push with long strings
TEST_F(CallStackTest, CallStackPushLongStringsTest) {
    CallStack callstack("Test error");
    
    std::string longModule(1000, 'M');
    std::string longFunction(1000, 'F');
    std::string longFile(1000, 'P');
    
    callstack.push(longModule, longFunction, longFile, 999);
    
    auto& stack = callstack.getStack();
    EXPECT_EQ(stack.size(), 1) << "Stack should have 1 frame";
    EXPECT_EQ(stack[0].module, longModule) << "Long module name should match";
    EXPECT_EQ(stack[0].function, longFunction) << "Long function name should match";
    EXPECT_EQ(stack[0].file, longFile) << "Long file name should match";
}

// Test CallStack getModuleName
TEST_F(CallStackTest, CallStackGetModuleNameTest) {
    CallStack callstack("Test error");
    
    callstack.push("first_module", "function1", "file1.cpp", 10);
    callstack.push("second_module", "function2", "file2.cpp", 20);
    
    EXPECT_EQ(callstack.getModuleName(), "first_module") << "Should return first module name";
}

// Test CallStack error time consistency
TEST_F(CallStackTest, CallStackErrorTimeConsistencyTest) {
    auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    CallStack callstack("Test error");
    
    auto after = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    int64_t error_time = callstack.getErrorTime();
    
    EXPECT_GE(error_time, before) << "Error time should be >= time before creation";
    EXPECT_LE(error_time, after) << "Error time should be <= time after creation";
}

// Test CallStack with realistic stack trace
TEST_F(CallStackTest, CallStackRealisticStackTraceTest) {
    CallStack callstack("NullPointerException: Cannot read property 'value' of null");
    
    callstack.push("libsystem_pthread.dylib", "_pthread_start", "", 0);
    callstack.push("app_module", "main", "/app/src/main.cpp", 150);
    callstack.push("app_module", "processRequest", "/app/src/handler.cpp", 75);
    callstack.push("app_module", "validateUser", "/app/src/validator.cpp", 42);
    callstack.push("app_module", "checkPermission", "/app/src/auth.cpp", 120);
    
    auto& stack = callstack.getStack();
    EXPECT_EQ(stack.size(), 5) << "Realistic stack should have 5 frames";
    EXPECT_EQ(callstack.getModuleName(), "libsystem_pthread.dylib") << "First module should be pthread library";
    EXPECT_EQ(stack[4].function, "checkPermission") << "Last function should be checkPermission";
}

// ========== Exception Tests ==========

// Test Exception creation with CallStack
TEST_F(CallStackTest, ExceptionCreationTest) {
    auto callstack = std::make_unique<CallStack>("Test exception");
    callstack->push("module", "function", "file.cpp", 10);
    
    Exception exception(std::move(callstack));
    
    EXPECT_GT(exception.getId(), 0) << "Exception ID should be greater than 0";
}

// Test Exception ID generation is unique
TEST_F(CallStackTest, ExceptionUniqueIdTest) {
    auto callstack1 = std::make_unique<CallStack>("Error 1");
    auto callstack2 = std::make_unique<CallStack>("Error 2");
    auto callstack3 = std::make_unique<CallStack>("Error 3");
    
    Exception exception1(std::move(callstack1));
    Exception exception2(std::move(callstack2));
    Exception exception3(std::move(callstack3));
    
    int32_t id1 = exception1.getId();
    int32_t id2 = exception2.getId();
    int32_t id3 = exception3.getId();
    
    EXPECT_NE(id1, id2) << "Exception IDs should be unique";
    EXPECT_NE(id2, id3) << "Exception IDs should be unique";
    EXPECT_NE(id1, id3) << "Exception IDs should be unique";
}

// Test Exception ID is sequential
TEST_F(CallStackTest, ExceptionSequentialIdTest) {
    auto callstack1 = std::make_unique<CallStack>("Error 1");
    auto callstack2 = std::make_unique<CallStack>("Error 2");
    
    Exception exception1(std::move(callstack1));
    int32_t id1 = exception1.getId();
    
    Exception exception2(std::move(callstack2));
    int32_t id2 = exception2.getId();
    
    EXPECT_EQ(id2, id1 + 1) << "Exception IDs should be sequential";
}

// Test Exception getCallStack returns valid CallStack
TEST_F(CallStackTest, ExceptionGetCallStackTest) {
    auto callstack = std::make_unique<CallStack>("Test exception");
    std::string expected_error = "Test exception";
    callstack->push("module", "function", "file.cpp", 10);
    
    Exception exception(std::move(callstack));
    auto retrieved_callstack = exception.getCallStack();
    
    ASSERT_NE(retrieved_callstack, nullptr) << "Retrieved callstack should not be null";
    EXPECT_EQ(retrieved_callstack->getErrorMessage(), expected_error) << "Error message should match";
    EXPECT_EQ(retrieved_callstack->getStack().size(), 1) << "Stack should have 1 frame";
    EXPECT_EQ(retrieved_callstack->getStack()[0].module, "module") << "Module should match";
}

// Test Exception getCallStack moves ownership
TEST_F(CallStackTest, ExceptionGetCallStackMovesOwnershipTest) {
    auto callstack = std::make_unique<CallStack>("Test exception");
    callstack->push("module", "function", "file.cpp", 10);
    
    Exception exception(std::move(callstack));
    
    // First call to getCallStack should succeed
    auto retrieved_callstack = exception.getCallStack();
    ASSERT_NE(retrieved_callstack, nullptr) << "First retrieval should return valid pointer";
    
    // Second call should return nullptr as ownership was moved
    auto second_retrieval = exception.getCallStack();
    EXPECT_EQ(second_retrieval, nullptr) << "Second retrieval should return nullptr after move";
}

// Test Exception with empty CallStack
TEST_F(CallStackTest, ExceptionWithEmptyCallStackTest) {
    auto callstack = std::make_unique<CallStack>("");
    
    Exception exception(std::move(callstack));
    
    EXPECT_GT(exception.getId(), 0) << "Exception ID should be valid";
    
    auto retrieved_callstack = exception.getCallStack();
    ASSERT_NE(retrieved_callstack, nullptr) << "Retrieved callstack should not be null";
    EXPECT_EQ(retrieved_callstack->getErrorMessage(), "") << "Error message should be empty";
    EXPECT_TRUE(retrieved_callstack->getStack().empty()) << "Stack should be empty";
}

// Test Exception with complex CallStack
TEST_F(CallStackTest, ExceptionWithComplexCallStackTest) {
    auto callstack = std::make_unique<CallStack>("ComplexException: Multiple nested calls failed");
    
    // Simulate a complex call stack
    for (int i = 0; i < 10; ++i) {
        callstack->push(
            "module_" + std::to_string(i),
            "function_" + std::to_string(i),
            "file_" + std::to_string(i) + ".cpp",
            i * 10
        );
    }
    
    Exception exception(std::move(callstack));
    
    auto retrieved_callstack = exception.getCallStack();
    ASSERT_NE(retrieved_callstack, nullptr) << "Retrieved callstack should not be null";
    EXPECT_EQ(retrieved_callstack->getStack().size(), 10) << "Stack should have 10 frames";
    
    // Verify first and last frames
    auto& stack = retrieved_callstack->getStack();
    EXPECT_EQ(stack[0].module, "module_0") << "First module should match";
    EXPECT_EQ(stack[9].module, "module_9") << "Last module should match";
    EXPECT_EQ(stack[9].line, 90) << "Last line number should match";
}

// Test multiple Exceptions concurrently (thread safety of ID generation)
TEST_F(CallStackTest, ExceptionConcurrentIdGenerationTest) {
    const int num_threads = 10;
    const int exceptions_per_thread = 100;
    std::vector<std::thread> threads;
    std::vector<int32_t> all_ids(num_threads * exceptions_per_thread);
    
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([t, &all_ids]() {
            for (int i = 0; i < exceptions_per_thread; ++i) {
                auto callstack = std::make_unique<CallStack>("Concurrent error");
                Exception exception(std::move(callstack));
                all_ids[t * exceptions_per_thread + i] = exception.getId();
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    // Check all IDs are unique
    std::sort(all_ids.begin(), all_ids.end());
    auto last = std::unique(all_ids.begin(), all_ids.end());
    size_t unique_count = std::distance(all_ids.begin(), last);
    
    EXPECT_EQ(unique_count, num_threads * exceptions_per_thread) 
        << "All exception IDs should be unique even with concurrent creation";
}

// ========== Integration Tests ==========

// Test complete workflow: create CallStack, add frames, wrap in Exception
TEST_F(CallStackTest, CompleteWorkflowTest) {
    // Create a CallStack
    auto callstack = std::make_unique<CallStack>("Workflow test exception");
    int64_t original_time = callstack->getErrorTime();
    
    // Add stack frames
    callstack->push("main_module", "main", "main.cpp", 100);
    callstack->push("handler_module", "handleRequest", "handler.cpp", 50);
    callstack->push("processor_module", "process", "processor.cpp", 25);
    
    // Wrap in Exception
    Exception exception(std::move(callstack));
    int32_t exception_id = exception.getId();
    
    // Retrieve and verify
    auto retrieved = exception.getCallStack();
    
    ASSERT_NE(retrieved, nullptr) << "Retrieved callstack should be valid";
    EXPECT_EQ(retrieved->getErrorMessage(), "Workflow test exception") << "Error message should be preserved";
    EXPECT_EQ(retrieved->getErrorTime(), original_time) << "Error time should be preserved";
    EXPECT_EQ(retrieved->getStack().size(), 3) << "All frames should be preserved";
    EXPECT_EQ(retrieved->getModuleName(), "main_module") << "Module name should be accessible";
    EXPECT_GT(exception_id, 0) << "Exception ID should be valid";
}

} // namespace pinpoint

