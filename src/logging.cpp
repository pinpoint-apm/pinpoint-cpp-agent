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

#include "logging.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>

namespace pinpoint {

    void shutdown_logger() {
        Logger::getInstance().shutdown();
    }

    void Logger::setLogLevel(const std::string& log_level) {
        const auto level = log_level.c_str();

        if (!strcasecmp(level, LOG_LEVEL_DEBUG)) {
            current_level_.store(static_cast<int>(LogLevel::kDebug), std::memory_order_relaxed);
        } else if (!strcasecmp(level, LOG_LEVEL_INFO)) {
            current_level_.store(static_cast<int>(LogLevel::kInfo), std::memory_order_relaxed);
        } else if (!strcasecmp(level, LOG_LEVEL_WARN) || !strcasecmp(level, LOG_LEVEL_WARN_ALIAS)) {
            current_level_.store(static_cast<int>(LogLevel::kWarn), std::memory_order_relaxed);
        } else if (!strcasecmp(level, LOG_LEVEL_ERROR)) {
            current_level_.store(static_cast<int>(LogLevel::kError), std::memory_order_relaxed);
        } else {
            // Keep the current level, but say so: a silently ignored typo
            // (e.g. "warnign") would otherwise look like a successful change,
            // especially on a config reload.
            LOG_WARN("unknown log level '{}'; keeping the current level", log_level);
        }
    }

    void Logger::setFileLogger(const std::string& log_file_path, const int max_size,
                               const int max_backups) {
        std::lock_guard<std::mutex> lock(mutex_);

        closed_ = false;
        file_broken_ = false;
        file_path_ = log_file_path;
        // max_size is the rotation threshold in MB. Guard against non-positive
        // values: a negative size cast to uint64_t and scaled by 1 MiB would
        // overflow into a garbage threshold (erratic or silently-disabled
        // rotation). Treat <= 0 as "rotation disabled" (max_file_size_ == 0),
        // matching rotateFileIfNeededLocked()'s existing semantics. A positive
        // int cannot overflow the multiply (INT_MAX * 2^20 < UINT64_MAX).
        max_file_size_ = max_size > 0
            ? static_cast<std::uint64_t>(max_size) * 1024 * 1024
            : 0;
        // < 1 means the one backup the agent kept before this was settable;
        // make_config() already clamps, this covers direct callers.
        max_backups_ = max_backups > 0 ? max_backups : 1;
        current_file_size_ = 0;
        file_enabled_ = false;
        file_stream_.reset();
        if (file_path_.empty()) {
            return;
        }

        std::error_code ec;
        const auto size = std::filesystem::file_size(file_path_, ec);
        if (!ec) {
            current_file_size_ = static_cast<std::uint64_t>(size);
        }

        // A failure here deliberately keeps the std::cout fallback rather than
        // latching file_broken_: this is configuration time, and a host that
        // just pointed the agent at an unusable path still wants to see why
        // its log file stays empty.
        openFileLocked();
    }

    // Opens file_path_ in append mode and reports whether the sink came up.
    // On failure the sink is left disabled and the caller decides what that
    // means — the std::cout fallback at configuration time, a dropped line
    // once the logger is closed or broken.
    bool Logger::openFileLocked() {
        file_stream_ = std::make_unique<std::ofstream>(file_path_, std::ios::out | std::ios::app);
        if (!file_stream_->is_open()) {
            file_stream_.reset();
            file_enabled_ = false;
            return false;
        }
        file_enabled_ = true;
        return true;
    }

    void Logger::setSink(LogSink sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        sink_ = std::move(sink);
        // Release: publishes sink_ to the acquire-load in write(), which reads
        // it before taking mutex_.
        sink_enabled_.store(static_cast<bool>(sink_), std::memory_order_release);
    }

    void Logger::shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);

        // Drop the host sink first. Unlike the file, it is not ours to keep
        // using: Shutdown() is where the host stops guaranteeing its logger is
        // alive, and a straggler calling into a torn-down callback is a
        // use-after-free in the host, not a lost log line. Stragglers fall
        // back to the file rules below.
        sink_enabled_.store(false, std::memory_order_release);
        sink_ = nullptr;

        if (file_stream_ && file_stream_->is_open()) {
            file_stream_->flush();
            file_stream_->close();
        }
        file_stream_.reset();
        // Bans the std::cout fallback from here on — it does not silence the
        // logger. A configured log file stays writable: write() reopens it in
        // append mode for stragglers, and the shutdown_logger() the teardown
        // paths run afterwards closes it again. Only a logger with no file to
        // write to goes quiet, which is what the stdout-only configuration
        // asks for once the agent is gone. Latched rather than assigned, so
        // the repeated shutdown_logger() calls on the teardown paths do not
        // un-close it.
        closed_ = true;
        file_enabled_ = false;
    }

    void Logger::write(LogLevel level, std::string_view file, int line, const std::string& message) {
        const char* level_str = level == LogLevel::kDebug ? LOG_LEVEL_DEBUG :
                                level == LogLevel::kInfo ? LOG_LEVEL_INFO :
                                level == LogLevel::kWarn ? LOG_LEVEL_WARN : LOG_LEVEL_ERROR;

        // A host sink takes the line outright: no file, no stdout, no
        // duplicates, and no timestamp of ours — the host's logger stamps its
        // own. Checked ahead of the timestamp and line assembly below so that
        // work is skipped entirely for hosts that took the log over.
        if (sink_enabled_.load(std::memory_order_acquire)) {
            const auto payload = fmt::format("[pinpoint][{}:{}] {}", file, line, message);
            std::lock_guard<std::mutex> lock(mutex_);
            if (sink_) {
                try {
                    sink_(level_str, payload.c_str());
                } catch (...) {
                    // A throwing host sink drops its line and nothing more. It
                    // must not take the process down, and must not spill the
                    // agent's diagnostics into a file or stdout the host asked
                    // us to leave alone.
                }
                return;
            }
            // Raced with setSink({}) / shutdown(): fall through to the file.
        }

        const auto now = std::chrono::system_clock::now();
        const auto now_time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tm{};
        localtime_r(&now_time, &tm);

        // Build the whole line in one formatting pass. fmt::memory_buffer uses
        // inline storage for typical lines and grows on the heap for longer
        // ones; this still avoids the ostringstream plus the chain of
        // temporary strings used previously. Output format is unchanged:
        // [YYYY-MM-DD HH:MM:SS.mmm][level][pinpoint][file:line] message
        fmt::memory_buffer buf;
        fmt::format_to(std::back_inserter(buf),
                       "[{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}][{}][pinpoint][{}:{}] {}\n",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()),
                       level_str, file, line, message);

        std::lock_guard<std::mutex> lock(mutex_);

        // A straggler logging after shutdown() closed the file: reopen it
        // rather than lose the line. What an overrunning teardown records on
        // its way out — the workers still draining, and the summary of how
        // long they took — is exactly what the log exists for. One attempt
        // per close: a failure latches file_broken_ so this does not retry an
        // ofstream construction per line.
        if (closed_ && !file_enabled_ && !file_broken_ && !file_path_.empty()) {
            file_broken_ = !openFileLocked();
        }

        if (file_enabled_ && file_stream_) {
            file_stream_->write(buf.data(), static_cast<std::streamsize>(buf.size()));
            // Flush every line. In pre-fork hosts several worker processes may
            // share one log file: with an empty stream buffer each line goes
            // out as a single O_APPEND write, so lines from sibling processes
            // cannot interleave mid-line, and a fork() can never duplicate
            // buffered-but-unwritten lines into every child. The log is a
            // low-volume diagnostic channel, so the extra write syscall per
            // line is irrelevant.
            file_stream_->flush();
            current_file_size_ += static_cast<std::uint64_t>(buf.size());
            rotateFileIfNeededLocked();
        } else if (!closed_ && !file_broken_) {
            std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        }
        // Otherwise the line is dropped. With no file left to write to, a
        // closed or broken logger must not start pouring the agent's
        // diagnostics into the host's stdout, which is not ours to use.
    }

    void Logger::rotateFileIfNeededLocked() {
        if (!file_enabled_ || !file_stream_ || max_file_size_ == 0) {
            return;
        }
        if (current_file_size_ < max_file_size_) {
            return;
        }

        file_stream_->flush();
        file_stream_->close();

        // Shift the backups down one index, dropping the oldest: .N is
        // removed, .N-1 becomes .N, ... , .1 becomes .2, and the live file
        // takes .1 below. The shifts are best effort — a missing .i is the
        // normal case before the ring has filled, and a failed one costs a
        // generation of history, nothing more — so only the live file's rename
        // is checked, exactly as when there was a single backup.
        std::error_code shift_ec;
        std::filesystem::remove(file_path_ + "." + std::to_string(max_backups_), shift_ec);
        for (int i = max_backups_ - 1; i >= 1; --i) {
            std::filesystem::rename(file_path_ + "." + std::to_string(i),
                                    file_path_ + "." + std::to_string(i + 1), shift_ec);
        }

        std::error_code rename_ec;
        const auto rotated_path = file_path_ + ".1";
        std::filesystem::rename(file_path_, rotated_path, rename_ec);

        if (!openFileLocked()) {
            // The old file was renamed away and its replacement will not open
            // (the directory lost write permission, the disk filled). Falling
            // back to std::cout here would dump the rest of the process's
            // logs into the host's stdout, so drop them instead and say so
            // once on stderr — this is the only notice there will be.
            // file_broken_ is latched and blocks the reopen in write(), so
            // rotation cannot run again and the notice cannot repeat.
            file_broken_ = true;
            std::cerr << "[pinpoint] log file " << file_path_
                      << " could not be reopened after rotation; further logs are dropped"
                      << std::endl;
            return;
        }

        if (rename_ec) {
            // The rename failed (e.g. no directory write permission, ".1" on
            // another filesystem), so the stream reopened the SAME unrenamed
            // file. Resetting the size counter to 0 would grow that file by
            // another max_file_size_ on every "rotation" forever; disable
            // rotation instead and say so once, so the size cap degrades to
            // a single over-full file rather than unbounded growth.
            max_file_size_ = 0;
            const std::string msg =
                "log rotation disabled: rename to " + rotated_path +
                " failed: " + rename_ec.message() + "\n";
            file_stream_->write(msg.data(), static_cast<std::streamsize>(msg.size()));
            // Same per-line flush policy as write().
            file_stream_->flush();
            return;
        }
        current_file_size_ = 0;
    }
}
