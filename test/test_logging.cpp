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

#include "../src/logging.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace pinpoint {

namespace {
    // Helper to read entire file contents
    std::string read_file(const std::string& path) {
        std::ifstream ifs(path);
        return {std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
    }
}

class LoggingTest : public ::testing::Test {
protected:
    void SetUp() override {
        log_file_ = std::filesystem::temp_directory_path() / "test_pinpoint_log.txt";
        rotated_file_ = log_file_.string() + ".1";
        cleanup();
    }

    void TearDown() override {
        Logger::getInstance().shutdown();
        // Reset to default info level for other tests
        Logger::getInstance().setLogLevel("info");
        cleanup();
    }

    void cleanup() {
        std::error_code ec;
        std::filesystem::remove(log_file_, ec);
        std::filesystem::remove(rotated_file_, ec);
    }

    std::filesystem::path log_file_;
    std::string rotated_file_;
};

// ========== kFileName Tests ==========

TEST(LoggingKFileNameTest, ExtractsFileNameFromUnixPath) {
    EXPECT_EQ(kFileName("/home/user/src/main.cpp"), "main.cpp");
}

TEST(LoggingKFileNameTest, ExtractsFileNameFromWindowsPath) {
    EXPECT_EQ(kFileName("C:\\Users\\user\\src\\main.cpp"), "main.cpp");
}

TEST(LoggingKFileNameTest, ReturnsFileNameWhenNoSlash) {
    EXPECT_EQ(kFileName("main.cpp"), "main.cpp");
}

TEST(LoggingKFileNameTest, EmptyPath) {
    EXPECT_EQ(kFileName(""), "");
}

TEST(LoggingKFileNameTest, TrailingSlash) {
    EXPECT_EQ(kFileName("/path/to/"), "");
}

// ========== setLogLevel Tests ==========

TEST_F(LoggingTest, SetLogLevelDebug) {
    Logger::getInstance().setLogLevel("debug");
    Logger::getInstance().setFileLogger(log_file_.string(), 10);

    Logger::getInstance().logDebug("test.cpp", 1, "debug message");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("debug message") != std::string::npos);
}

TEST_F(LoggingTest, SetLogLevelInfo) {
    Logger::getInstance().setLogLevel("info");
    Logger::getInstance().setFileLogger(log_file_.string(), 10);

    Logger::getInstance().logDebug("test.cpp", 1, "debug hidden");
    Logger::getInstance().logInfo("test.cpp", 2, "info visible");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("debug hidden") == std::string::npos);
    EXPECT_TRUE(content.find("info visible") != std::string::npos);
}

TEST_F(LoggingTest, SetLogLevelWarn) {
    Logger::getInstance().setLogLevel("warning");
    Logger::getInstance().setFileLogger(log_file_.string(), 10);

    Logger::getInstance().logInfo("test.cpp", 1, "info hidden");
    Logger::getInstance().logWarn("test.cpp", 2, "warn visible");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("info hidden") == std::string::npos);
    EXPECT_TRUE(content.find("warn visible") != std::string::npos);
}

TEST_F(LoggingTest, SetLogLevelError) {
    Logger::getInstance().setLogLevel("error");
    Logger::getInstance().setFileLogger(log_file_.string(), 10);

    Logger::getInstance().logWarn("test.cpp", 1, "warn hidden");
    Logger::getInstance().logError("test.cpp", 2, "error visible");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("warn hidden") == std::string::npos);
    EXPECT_TRUE(content.find("error visible") != std::string::npos);
}

TEST_F(LoggingTest, SetLogLevelCaseInsensitive) {
    Logger::getInstance().setLogLevel("DEBUG");
    Logger::getInstance().setFileLogger(log_file_.string(), 10);

    Logger::getInstance().logDebug("test.cpp", 1, "debug after uppercase");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("debug after uppercase") != std::string::npos);
}

TEST_F(LoggingTest, SetLogLevelInvalidKeepsCurrent) {
    Logger::getInstance().setLogLevel("info");
    Logger::getInstance().setLogLevel("invalid_level");
    Logger::getInstance().setFileLogger(log_file_.string(), 10);

    // Should still be at info level since invalid level is ignored
    Logger::getInstance().logInfo("test.cpp", 1, "still info");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("still info") != std::string::npos);
}

// ========== setFileLogger Tests ==========

TEST_F(LoggingTest, SetFileLoggerCreatesFile) {
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().logInfo("test.cpp", 1, "file test");
    Logger::getInstance().shutdown();

    EXPECT_TRUE(std::filesystem::exists(log_file_));
    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("file test") != std::string::npos);
}

TEST_F(LoggingTest, SetFileLoggerEmptyPathDisables) {
    Logger::getInstance().setFileLogger("", 10);
    // Should not crash; logging goes to stdout instead
    Logger::getInstance().logInfo("test.cpp", 1, "stdout message");
    // Just verify no crash — no file created
    EXPECT_FALSE(std::filesystem::exists(log_file_));
}

TEST_F(LoggingTest, SetFileLoggerAppendsToExistingFile) {
    // Write initial content
    {
        std::ofstream ofs(log_file_.string());
        ofs << "existing content\n";
    }

    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().logInfo("test.cpp", 1, "appended message");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("existing content") != std::string::npos);
    EXPECT_TRUE(content.find("appended message") != std::string::npos);
}

TEST_F(LoggingTest, SetFileLoggerReconfigures) {
    auto log_file_2 = std::filesystem::temp_directory_path() / "test_pinpoint_log_2.txt";

    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().logInfo("test.cpp", 1, "first file msg");

    Logger::getInstance().setFileLogger(log_file_2.string(), 10);
    Logger::getInstance().logInfo("test.cpp", 2, "second file msg");
    Logger::getInstance().shutdown();

    auto content1 = read_file(log_file_.string());
    auto content2 = read_file(log_file_2.string());
    EXPECT_TRUE(content1.find("first file msg") != std::string::npos);
    EXPECT_TRUE(content2.find("second file msg") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(log_file_2, ec);
}

// ========== write / Output Format Tests ==========

TEST_F(LoggingTest, WriteOutputContainsTimestamp) {
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().logInfo("test.cpp", 42, "timestamp check");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    // Should contain date pattern like [2026-
    EXPECT_TRUE(content.find("[20") != std::string::npos);
}

TEST_F(LoggingTest, WriteOutputContainsLogLevel) {
    Logger::getInstance().setLogLevel("debug");
    Logger::getInstance().setFileLogger(log_file_.string(), 10);

    Logger::getInstance().logDebug("test.cpp", 1, "d");
    Logger::getInstance().logInfo("test.cpp", 2, "i");
    Logger::getInstance().logWarn("test.cpp", 3, "w");
    Logger::getInstance().logError("test.cpp", 4, "e");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("[debug]") != std::string::npos);
    EXPECT_TRUE(content.find("[info]") != std::string::npos);
    EXPECT_TRUE(content.find("[warning]") != std::string::npos);
    EXPECT_TRUE(content.find("[error]") != std::string::npos);
}

TEST_F(LoggingTest, WriteOutputContainsPinpointTag) {
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().logInfo("test.cpp", 1, "tag check");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("[pinpoint]") != std::string::npos);
}

TEST_F(LoggingTest, WriteOutputContainsFileAndLine) {
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().logInfo("my_source.cpp", 99, "location check");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("[my_source.cpp:99]") != std::string::npos);
}

TEST_F(LoggingTest, WriteOutputContainsMessage) {
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().logInfo("test.cpp", 1, "hello world 12345");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("hello world 12345") != std::string::npos);
}

// ========== fmt formatting Tests ==========

TEST_F(LoggingTest, FmtFormatArgs) {
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().logInfo("test.cpp", 1, "count={} name={}", 42, "test");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("count=42 name=test") != std::string::npos);
}

TEST_F(LoggingTest, FmtFormatErrorHandled) {
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    // Mismatched format: more placeholders than args — should not crash
    Logger::getInstance().logInfo("test.cpp", 1, "value={} extra={}", 1);
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    // Should contain "log format error" from the catch block
    EXPECT_TRUE(content.find("log format error") != std::string::npos);
}

// ========== Log Rotation Tests ==========

TEST_F(LoggingTest, RotateFileWhenExceedingMaxSize) {
    // Set max size to 1 MB; we'll use a small value via direct file size manipulation
    // max_size param is in MB, so use the smallest: 1 MB
    Logger::getInstance().setLogLevel("debug");
    Logger::getInstance().setFileLogger(log_file_.string(), 1); // 1 MB

    // Write enough data to exceed 1 MB
    std::string large_msg(1024, 'X'); // 1 KB per message
    for (int i = 0; i < 1100; ++i) {
        Logger::getInstance().logInfo("test.cpp", 1, "{}", large_msg);
    }
    Logger::getInstance().shutdown();

    // The rotated file should exist
    EXPECT_TRUE(std::filesystem::exists(rotated_file_));
    // The original file should also exist (new file after rotation)
    EXPECT_TRUE(std::filesystem::exists(log_file_));
}

TEST_F(LoggingTest, RotatedFileReplacedOnSecondRotation) {
    Logger::getInstance().setLogLevel("debug");
    Logger::getInstance().setFileLogger(log_file_.string(), 1); // 1 MB

    std::string large_msg(1024, 'A');
    // First rotation
    for (int i = 0; i < 1100; ++i) {
        Logger::getInstance().logInfo("test.cpp", 1, "{}", large_msg);
    }
    EXPECT_TRUE(std::filesystem::exists(rotated_file_));

    // Second rotation should overwrite .1 file
    std::string large_msg2(1024, 'B');
    for (int i = 0; i < 1100; ++i) {
        Logger::getInstance().logInfo("test.cpp", 1, "{}", large_msg2);
    }
    Logger::getInstance().shutdown();

    EXPECT_TRUE(std::filesystem::exists(rotated_file_));
    auto rotated_content = read_file(rotated_file_);
    // The rotated file should contain 'B' messages (from the second batch that was rotated)
    EXPECT_TRUE(rotated_content.find('B') != std::string::npos);
}

// ========== shutdown Tests ==========

TEST_F(LoggingTest, ShutdownFlushesAndCloses) {
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().logInfo("test.cpp", 1, "before shutdown");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("before shutdown") != std::string::npos);
}

TEST_F(LoggingTest, ShutdownCanBeCalledMultipleTimes) {
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().logInfo("test.cpp", 1, "msg");
    Logger::getInstance().shutdown();
    Logger::getInstance().shutdown(); // Should not crash
    Logger::getInstance().shutdown();
}

TEST_F(LoggingTest, ShutdownLogger) {
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().logInfo("test.cpp", 1, "via global shutdown");
    shutdown_logger();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("via global shutdown") != std::string::npos);
}

// ========== Thread Safety Tests ==========

TEST_F(LoggingTest, ConcurrentWrites) {
    Logger::getInstance().setLogLevel("debug");
    Logger::getInstance().setFileLogger(log_file_.string(), 10);

    constexpr int threads_count = 4;
    constexpr int msgs_per_thread = 100;
    std::vector<std::thread> threads;

    for (int t = 0; t < threads_count; ++t) {
        threads.emplace_back([t]() {
            for (int i = 0; i < msgs_per_thread; ++i) {
                Logger::getInstance().logInfo("test.cpp", t, "thread={} msg={}", t, i);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    // Verify all threads wrote messages (check at least first and last thread)
    EXPECT_TRUE(content.find("thread=0") != std::string::npos);
    EXPECT_TRUE(content.find(fmt::format("thread={}", threads_count - 1)) != std::string::npos);

    // Count newlines to verify approximate number of messages
    auto line_count = std::count(content.begin(), content.end(), '\n');
    EXPECT_EQ(line_count, threads_count * msgs_per_thread);
}

// ========== LOG_* Macro Tests ==========

TEST_F(LoggingTest, LogMacrosWork) {
    Logger::getInstance().setLogLevel("debug");
    Logger::getInstance().setFileLogger(log_file_.string(), 10);

    LOG_DEBUG("macro debug {}", 1);
    LOG_INFO("macro info {}", 2);
    LOG_WARN("macro warn {}", 3);
    LOG_ERROR("macro error {}", 4);
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("macro debug 1") != std::string::npos);
    EXPECT_TRUE(content.find("macro info 2") != std::string::npos);
    EXPECT_TRUE(content.find("macro warn 3") != std::string::npos);
    EXPECT_TRUE(content.find("macro error 4") != std::string::npos);
    // Macros should inject this test file's name
    EXPECT_TRUE(content.find("test_logging.cpp") != std::string::npos);
}

} // namespace pinpoint
