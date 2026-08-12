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

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "utility.h"

namespace pinpoint {
 
    /// @brief Captures a single frame in a collected call stack.
    typedef struct {
        std::string module;
        std::string function;
        std::string file;
        int line;
    } StackFrame;
 
    /// @brief Collects stack frames and contextual information for a captured exception.
    class CallStack {
    public:
        CallStack(std::string_view error_message) : error_message_(truncated(error_message, kMaxErrorMessageLength)),
                                               error_time_{to_milli_seconds(std::chrono::system_clock::now())},
                                               stack_{} {}

        /// @brief Adds a frame to the call stack.
        void push(std::string_view module, std::string_view function, std::string_view file, int line) {
            // Frames come from user-controlled readers with no depth limit of
            // their own; cap both the frame count and the per-field string
            // length so one pathological callstack cannot pin unbounded
            // memory in the span's exception buffer.
            if (stack_.size() >= kMaxFrames) {
                return;
            }
            if (stack_.capacity() == 0) {
                stack_.reserve(kInitialFrameCapacity);
            }
            stack_.emplace_back(StackFrame{truncated(module, kMaxFrameStringLength),
                                           truncated(function, kMaxFrameStringLength),
                                           truncated(file, kMaxFrameStringLength), line});
        }

        /**
         * @brief Returns the recorded error message.
         *
         * @return Mutable reference to the message.
         */
        const std::string& getErrorMessage() const {
            return error_message_;
        }

        /// @brief Returns the timestamp of the error in milliseconds.
        int64_t getErrorTime() const {
            return error_time_;
        }

        /// @brief Returns the collected stack frames.
        const std::vector<StackFrame>& getStack() const {
            return stack_;
        }

        /// @brief Convenience accessor for the module name of the top frame.
        const std::string& getModuleName() const {
            static const std::string empty;
            if (stack_.empty()) {
                return empty;
            }
            return stack_[0].module;
        }

    private:
        static constexpr size_t kMaxFrames = 128;
        // Typical callstacks are a couple dozen frames deep; reserving up
        // front avoids repeated reallocations on the error path without
        // paying for kMaxFrames when stacks are shallow.
        static constexpr size_t kInitialFrameCapacity = 16;
        static constexpr size_t kMaxFrameStringLength = 1024;
        static constexpr size_t kMaxErrorMessageLength = 4096;

        // The strings end up in protobuf string fields, which require valid
        // UTF-8: never cut mid-character.
        static std::string truncated(std::string_view s, size_t max_length) {
            return std::string(s.substr(0, utf8SafeCutLength(s, max_length)));
        }

        std::string error_message_;
        int64_t error_time_;
        std::vector<StackFrame> stack_;
    };

    /// @brief Wraps a captured call stack with an identifier suitable for transmission.
    class Exception {
    public:
        Exception(std::unique_ptr<CallStack> callstack) : id_{exception_id_gen.fetch_add(1)}, callstack_(std::move(callstack)) {}

        /// @brief Returns the generated exception identifier.
        int64_t getId() const { return id_; }
        /// @brief Returns a reference to the captured call stack.
        const CallStack& getCallStack() const { return *callstack_; }

        // 64-bit to match the PException.exceptionId proto field: a 32-bit
        // counter would wrap into negative/duplicate ids on long-lived,
        // high-error-rate agents.
        static std::atomic<int64_t> exception_id_gen;

    private:
        int64_t id_;
        std::unique_ptr<CallStack> callstack_;
    };

} // namespace pinpoint
