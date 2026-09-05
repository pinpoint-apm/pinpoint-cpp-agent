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
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pinpoint {

namespace {
    // Helper to read entire file contents
    std::string read_file(const std::string& path) {
        std::ifstream ifs(path);
        return {std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
    }

    size_t count_occurrences(const std::string& haystack, const std::string& needle) {
        size_t count = 0;
        for (auto pos = haystack.find(needle); pos != std::string::npos;
             pos = haystack.find(needle, pos + needle.size())) {
            ++count;
        }
        return count;
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
        // Reset to default info level for other tests, and back to stdout mode
        // so the closed-after-shutdown drop does not silence them.
        Logger::getInstance().setFileLogger("", 0);
        Logger::getInstance().setLogLevel("info");
        cleanup();
    }

    void cleanup() {
        std::error_code ec;
        std::filesystem::remove(log_file_, ec);
        // .1 through .4: enough for the multi-backup rotation tests.
        for (int i = 1; i <= 4; ++i) {
            std::filesystem::remove(log_file_.string() + "." + std::to_string(i), ec);
        }
    }

    // Writes past the 1 MB rotation threshold once.
    static void fill_one_rotation(char fill) {
        const std::string large_msg(1024, fill);
        for (int i = 0; i < 1100; ++i) {
            Logger::getInstance().logInfo("test.cpp", 1, "{}", large_msg);
        }
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

// Every line is flushed as it is written (multi-process shared-file safety:
// no buffered bytes can be duplicated by fork() or interleave mid-line), so
// it must be visible in the file BEFORE any shutdown/flush call.
TEST_F(LoggingTest, WriteFlushesEachLineImmediately) {
    Logger::getInstance().setFileLogger(log_file_.string(), 10);

    Logger::getInstance().logInfo("test.cpp", 1, "flushed immediately");

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("flushed immediately") != std::string::npos)
        << "a written line must reach the file without an explicit flush";
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

TEST_F(LoggingTest, SetLogLevelInvalidWarns) {
    Logger::getInstance().setLogLevel("info");
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().setLogLevel("warnign");  // typo, not an accepted alias
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("unknown log level 'warnign'") != std::string::npos);
}

// "warn" is what Go/logrus and Java configurations say; it must select the
// warning level rather than being rejected as a typo.
TEST_F(LoggingTest, SetLogLevelAcceptsWarnAlias) {
    Logger::getInstance().setLogLevel("info");
    Logger::getInstance().setLogLevel("warn");
    Logger::getInstance().setFileLogger(log_file_.string(), 10);

    Logger::getInstance().logInfo("test.cpp", 1, "info below the level");
    Logger::getInstance().logWarn("test.cpp", 2, "warn at the level");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_EQ(content.find("info below the level"), std::string::npos);
    EXPECT_NE(content.find("warn at the level"), std::string::npos);
    // The alias is input-only: lines stay labelled "warning".
    EXPECT_NE(content.find("[warning]"), std::string::npos);
    EXPECT_EQ(content.find("unknown log level"), std::string::npos);
}

TEST_F(LoggingTest, SetLogLevelAcceptsWarnAliasCaseInsensitively) {
    Logger::getInstance().setLogLevel("info");
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().setLogLevel("WARN");
    Logger::getInstance().logInfo("test.cpp", 1, "info below the level");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_EQ(content.find("unknown log level"), std::string::npos);
    EXPECT_EQ(content.find("info below the level"), std::string::npos);
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

// A straggler logging after shutdown must not fall back to std::cout — the
// agent is embedded in hosts whose stdout belongs to the host, not to us --
// but it still belongs in the log file. An overrunning teardown reports how
// it ended after shutdown_logger() has already run, and that report is the
// whole reason the deadline is observable at all.
TEST_F(LoggingTest, FileLoggerKeepsWritingToTheFileAfterShutdown) {
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().logInfo("test.cpp", 1, "before shutdown");
    Logger::getInstance().shutdown();

    std::cout.flush();
    testing::internal::CaptureStdout();
    Logger::getInstance().logInfo("test.cpp", 2, "after shutdown straggler");
    std::cout.flush();
    const auto captured = testing::internal::GetCapturedStdout();

    EXPECT_TRUE(captured.empty()) << "leaked to stdout: " << captured;
    // The reopened stream flushes per line like any other, so the straggler
    // is on disk before anything closes the file again.
    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("before shutdown") != std::string::npos);
    EXPECT_TRUE(content.find("after shutdown straggler") != std::string::npos)
        << "the straggler was dropped instead of reaching the file";

    // The second shutdown_logger() the teardown paths run once the stragglers
    // are done closes the reopened stream for good.
    shutdown_logger();
    EXPECT_TRUE(read_file(log_file_.string()).find("after shutdown straggler")
                != std::string::npos);
}

// Regression: a file logger whose file will not open falls back to std::cout
// for the rest of the process, with file_enabled_ false. Shutdown must
// silence it all the same — it is still a file logger, and it is the one
// whose stragglers would otherwise keep landing on the host's stdout.
TEST_F(LoggingTest, UnopenableFileLoggerDropsLogsAfterShutdown) {
    const auto missing_dir = std::filesystem::temp_directory_path() / "pinpoint_no_such_dir";
    std::error_code ec;
    std::filesystem::remove_all(missing_dir, ec);
    ASSERT_FALSE(std::filesystem::exists(missing_dir));

    Logger::getInstance().setFileLogger((missing_dir / "agent.log").string(), 10);

    // The fallback is in force: the line goes to stdout while the agent runs.
    std::cout.flush();
    testing::internal::CaptureStdout();
    Logger::getInstance().logInfo("test.cpp", 1, "while running");
    std::cout.flush();
    EXPECT_NE(testing::internal::GetCapturedStdout().find("while running"), std::string::npos)
        << "a failed log file should degrade to stdout, not to silence";

    Logger::getInstance().shutdown();

    std::cout.flush();
    testing::internal::CaptureStdout();
    Logger::getInstance().logInfo("test.cpp", 2, "after shutdown straggler");
    std::cout.flush();
    const auto captured = testing::internal::GetCapturedStdout();

    EXPECT_TRUE(captured.empty()) << "leaked to the host's stdout: " << captured;
}

// The default sink with no Log.FilePath configured is stdout, so a closed
// logger has nowhere left to put a line — and the host's stdout is not an
// answer once the agent has said goodbye. Drop it.
TEST_F(LoggingTest, StdoutLoggerDropsLogsAfterShutdown) {
    Logger::getInstance().setFileLogger("", 0);
    Logger::getInstance().shutdown();

    std::cout.flush();
    testing::internal::CaptureStdout();
    Logger::getInstance().logInfo("test.cpp", 1, "after shutdown on stdout");
    std::cout.flush();
    const auto captured = testing::internal::GetCapturedStdout();

    EXPECT_TRUE(captured.empty()) << "leaked to the host's stdout: " << captured;
}

// Rotation renames the log away and reopens the path. When that reopen fails
// the sink is gone for good, and the pre-existing std::cout fallback would
// then pour every later line into the host's stdout for the rest of the
// process. Drop them instead, after a single notice on stderr.
TEST_F(LoggingTest, RotationReopenFailureDropsLogsAndNotifiesOnce) {
    const auto dir = std::filesystem::temp_directory_path() / "pinpoint_rotation_reopen";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto path = (dir / "agent.log").string();

    // Start already over the 1 MB rotation threshold, so the next line
    // rotates without having to write a megabyte through the logger.
    {
        std::ofstream ofs(path);
        ofs << std::string(2 * 1024 * 1024, 'x');
    }
    Logger::getInstance().setFileLogger(path, 1);

    // Pull the directory out from under the open stream: writes to the
    // unlinked inode still succeed, the rename does not, and neither does the
    // reopen — which is the path under test.
    std::filesystem::remove_all(dir, ec);
    ASSERT_FALSE(std::filesystem::exists(dir));

    std::cout.flush();
    std::cerr.flush();
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    Logger::getInstance().logInfo("test.cpp", 1, "the line that rotates");
    for (int i = 0; i < 5; ++i) {
        Logger::getInstance().logInfo("test.cpp", 2, "after the broken rotation {}", i);
    }
    std::cout.flush();
    std::cerr.flush();
    const auto out = testing::internal::GetCapturedStdout();
    const auto err = testing::internal::GetCapturedStderr();

    EXPECT_TRUE(out.empty()) << "leaked to the host's stdout: " << out;
    EXPECT_EQ(count_occurrences(err, "could not be reopened after rotation"), 1u)
        << "the failure must be reported exactly once; stderr was:\n" << err;

    std::filesystem::remove_all(dir, ec);
}

// Restart (a second StartAgent) reconfigures the file logger, which must lift
// the closed state instead of leaving the agent permanently mute.
TEST_F(LoggingTest, SetFileLoggerReopensAfterShutdown) {
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().shutdown();
    // Repeated shutdowns (do_shutdown plus the teardown runner) stay closed.
    Logger::getInstance().shutdown();

    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().logInfo("test.cpp", 1, "after restart");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("after restart") != std::string::npos);
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

// ========== Log-site throttle (QueueDropReporter::acquire) / LOG_*_THROTTLED Tests ==========

TEST(LogThrottleTest, FirstOccurrenceAlwaysReports) {
    QueueDropReporter throttle;
    EXPECT_EQ(throttle.acquire(), 1u);
}

TEST(LogThrottleTest, SuppressesRepeatsWithinInterval) {
    QueueDropReporter throttle;  // default 60s interval: the test stays inside it
    ASSERT_EQ(throttle.acquire(), 1u);
    EXPECT_EQ(throttle.acquire(), 0u);
    EXPECT_EQ(throttle.acquire(), 0u);
}

TEST(LogThrottleTest, FoldsSuppressedOccurrencesIntoNextReport) {
    // The window must be far wider than any plausible scheduler preemption
    // between the acquire() calls below: with a tight window (e.g. 50ms), a
    // loaded CI or sanitizer build can stall long enough for a "suppressed"
    // call to land in a fresh window and spuriously win it.
    QueueDropReporter throttle(std::chrono::milliseconds(500));
    ASSERT_EQ(throttle.acquire(), 1u);
    EXPECT_EQ(throttle.acquire(), 0u);
    EXPECT_EQ(throttle.acquire(), 0u);
    EXPECT_EQ(throttle.acquire(), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    // The next report covers the three suppressed occurrences plus itself.
    EXPECT_EQ(throttle.acquire(), 4u);
}

TEST(LogThrottleTest, ExactlyOneConcurrentCallerWinsTheWindow) {
    QueueDropReporter throttle;  // default 60s interval: only the first window grants
    constexpr int kThreads = 8;
    constexpr int kCallsPerThread = 100;
    std::atomic<int> winners{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&throttle, &winners] {
            for (int i = 0; i < kCallsPerThread; ++i) {
                if (throttle.acquire() > 0) {
                    winners.fetch_add(1);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(winners.load(), 1);
}

TEST_F(LoggingTest, ThrottledMacroLogsOncePerCallSite) {
    Logger::getInstance().setLogLevel("warning");
    Logger::getInstance().setFileLogger(log_file_.string(), 10);

    for (int i = 0; i < 10; ++i) {
        LOG_WARN_THROTTLED("throttled warn once");
    }
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_EQ(count_occurrences(content, "throttled warn once"), 1u)
        << "repeats at one call site within the interval must be suppressed";
}

TEST_F(LoggingTest, ThrottledLogAppendsFoldedOccurrenceCount) {
    Logger::getInstance().setLogLevel("warning");
    Logger::getInstance().setFileLogger(log_file_.string(), 10);

    Logger::getInstance().logWarnThrottled(1, "test.cpp", 1, "single occurrence");
    Logger::getInstance().logWarnThrottled(5, "test.cpp", 2, "repeated occurrence");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_TRUE(content.find("single occurrence") != std::string::npos);
    EXPECT_TRUE(content.find("single occurrence [") == std::string::npos)
        << "a single occurrence must not carry a folded count";
    EXPECT_TRUE(content.find("repeated occurrence [5 occurrences since last report]")
                != std::string::npos);
}

// ========== Host log sink (AgentOptions::log_sink) Tests ==========

TEST_F(LoggingTest, SinkTakesTheLineInsteadOfTheFile) {
    std::vector<std::pair<std::string, std::string>> captured;
    std::mutex captured_mutex;

    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().setSink([&](const char* level, const char* message) {
        std::lock_guard<std::mutex> lock(captured_mutex);
        captured.emplace_back(level, message);
    });

    Logger::getInstance().logWarn("some/dir/test.cpp", 42, "to the host {}", "logger");
    Logger::getInstance().shutdown();

    ASSERT_EQ(captured.size(), 1u);
    EXPECT_EQ(captured[0].first, "warning");
    EXPECT_EQ(captured[0].second, "[pinpoint][some/dir/test.cpp:42] to the host logger");
    // No agent timestamp, no trailing newline: the host's logger stamps its own.
    EXPECT_EQ(captured[0].second.find('\n'), std::string::npos);
    // And the configured file must not receive a duplicate.
    EXPECT_EQ(read_file(log_file_.string()).find("to the host logger"), std::string::npos);
}

TEST_F(LoggingTest, SinkRespectsTheLogLevel) {
    std::atomic<int> calls{0};
    Logger::getInstance().setLogLevel("warning");
    Logger::getInstance().setSink([&](const char*, const char*) { calls.fetch_add(1); });

    Logger::getInstance().logInfo("test.cpp", 1, "below the level");
    Logger::getInstance().logWarn("test.cpp", 2, "at the level");
    Logger::getInstance().shutdown();

    EXPECT_EQ(calls.load(), 1);
}

TEST_F(LoggingTest, ThrowingSinkDropsOnlyItsLine) {
    std::atomic<int> calls{0};
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().setSink([&](const char*, const char*) {
        calls.fetch_add(1);
        throw std::runtime_error("host sink blew up");
    });

    Logger::getInstance().logWarn("test.cpp", 1, "first line");
    Logger::getInstance().logWarn("test.cpp", 2, "second line");
    Logger::getInstance().shutdown();

    EXPECT_EQ(calls.load(), 2) << "a throwing sink must not disable itself";
    // Not spilled into the file the host asked us to leave alone either.
    EXPECT_EQ(read_file(log_file_.string()).find("first line"), std::string::npos);
}

TEST_F(LoggingTest, ShutdownClearsTheSink) {
    std::atomic<int> calls{0};
    Logger::getInstance().setSink([&](const char*, const char*) { calls.fetch_add(1); });
    Logger::getInstance().shutdown();

    // A straggler logging after Shutdown() must not reach a host callback whose
    // state may already be gone.
    Logger::getInstance().logWarn("test.cpp", 1, "straggler");
    EXPECT_EQ(calls.load(), 0);
}

TEST_F(LoggingTest, ClearingTheSinkRestoresTheFile) {
    Logger::getInstance().setFileLogger(log_file_.string(), 10);
    Logger::getInstance().setSink([](const char*, const char*) {});
    Logger::getInstance().logWarn("test.cpp", 1, "to the sink");
    Logger::getInstance().setSink({});
    Logger::getInstance().logWarn("test.cpp", 2, "back to the file");
    Logger::getInstance().shutdown();

    auto content = read_file(log_file_.string());
    EXPECT_EQ(content.find("to the sink"), std::string::npos);
    EXPECT_NE(content.find("back to the file"), std::string::npos);
}

// ========== Log.MaxBackups Tests ==========

// With N backups each rotation shifts the ring down one index and drops the
// oldest, so the newest rotated content is in .1 and nothing survives past .N.
// Tracked with a one-off marker per generation rather than the bulk filler:
// a fill overshoots the threshold, so its tail spills into the next generation
// and only the marker identifies a generation unambiguously.
TEST_F(LoggingTest, KeepsMaxBackupsRotatedFiles) {
    Logger::getInstance().setLogLevel("debug");
    Logger::getInstance().setFileLogger(log_file_.string(), 1, 3);

    for (int generation = 1; generation <= 4; ++generation) {
        Logger::getInstance().logInfo("test.cpp", 1, "GENERATION-{}-MARKER", generation);
        fill_one_rotation('X');
    }
    Logger::getInstance().shutdown();

    const auto backup = [this](int i) { return log_file_.string() + "." + std::to_string(i); };
    ASSERT_TRUE(std::filesystem::exists(backup(1)));
    ASSERT_TRUE(std::filesystem::exists(backup(2)));
    ASSERT_TRUE(std::filesystem::exists(backup(3)));
    EXPECT_FALSE(std::filesystem::exists(backup(4))) << "the cap must be honoured";

    // Newest first.
    EXPECT_NE(read_file(backup(1)).find("GENERATION-4-MARKER"), std::string::npos);
    EXPECT_NE(read_file(backup(2)).find("GENERATION-3-MARKER"), std::string::npos);
    EXPECT_NE(read_file(backup(3)).find("GENERATION-2-MARKER"), std::string::npos);
    // The generation pushed past the cap is deleted, not left behind anywhere.
    for (int i = 1; i <= 3; ++i) {
        EXPECT_EQ(read_file(backup(i)).find("GENERATION-1-MARKER"), std::string::npos)
            << "the oldest generation must be deleted, not kept in ." << i;
    }
}

// The default, and anything below 1, is the single backup the agent kept
// before the setting existed.
TEST_F(LoggingTest, MaxBackupsBelowOneKeepsOneBackup) {
    Logger::getInstance().setLogLevel("debug");
    Logger::getInstance().setFileLogger(log_file_.string(), 1, 0);

    Logger::getInstance().logInfo("test.cpp", 1, "GENERATION-1-MARKER");
    fill_one_rotation('X');
    Logger::getInstance().logInfo("test.cpp", 1, "GENERATION-2-MARKER");
    fill_one_rotation('X');
    Logger::getInstance().shutdown();

    EXPECT_TRUE(std::filesystem::exists(rotated_file_));
    EXPECT_FALSE(std::filesystem::exists(log_file_.string() + ".2"));
    EXPECT_NE(read_file(rotated_file_).find("GENERATION-2-MARKER"), std::string::npos);
}

} // namespace pinpoint
