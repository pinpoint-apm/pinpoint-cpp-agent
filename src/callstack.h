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
 
    /**
     * @brief Captures a single frame in a collected call stack.
     */
    typedef struct {
        std::string module;
        std::string function;
        std::string file;
        int line;
    } StackFrame;
 
    /**
     * @brief Collects stack frames and contextual information for a captured exception.
     */
    class CallStack {
    public:
        CallStack(std::string_view error_message) : error_message_(truncated(error_message, kMaxErrorMessageLength)),
                                               error_time_{to_milli_seconds(std::chrono::system_clock::now())},
                                               stack_{} {}
        //~CallStack() = default;

        /**
         * @brief Adds a frame to the call stack.
         *
         * @param module Module or library name.
         * @param function Function name.
         * @param file Source file path.
         * @param line Line number within the file.
         */
        void push(std::string_view module, std::string_view function, std::string_view file, int line) {
            // Frames come from user-controlled readers with no depth limit of
            // their own; cap both the frame count and the per-field string
            // length so one pathological callstack cannot pin unbounded
            // memory in the span's exception buffer.
            if (stack_.size() >= kMaxFrames) {
                return;
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

        /**
         * @brief Returns the timestamp of the error in milliseconds.
         */
        int64_t getErrorTime() const {
            return error_time_;
        }

        /**
         * @brief Returns the collected stack frames.
         */
        const std::vector<StackFrame>& getStack() const {
            return stack_;
        }

        /**
         * @brief Convenience accessor for the module name of the top frame.
         */
        const std::string& getModuleName() const {
            static const std::string empty;
            if (stack_.empty()) {
                return empty;
            }
            return stack_[0].module;
        }

    private:
        static constexpr size_t kMaxFrames = 128;
        static constexpr size_t kMaxFrameStringLength = 1024;
        static constexpr size_t kMaxErrorMessageLength = 4096;

        static std::string truncated(std::string_view s, size_t max_length) {
            if (s.size() > max_length) {
                s = s.substr(0, max_length);
                // The strings end up in protobuf string fields, which require
                // valid UTF-8: if the cut split a multi-byte sequence, drop
                // the incomplete trailing character.
                size_t tail = 0;
                while (tail < s.size() && tail < 3 &&
                       (static_cast<unsigned char>(s[s.size() - 1 - tail]) & 0xC0) == 0x80) {
                    ++tail;
                }
                if (tail < s.size()) {
                    const auto lead = static_cast<unsigned char>(s[s.size() - 1 - tail]);
                    const size_t expected = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
                    if (expected > tail + 1) {
                        s.remove_suffix(tail + 1);
                    }
                }
            }
            return std::string(s);
        }

        std::string error_message_;
        int64_t error_time_;
        std::vector<StackFrame> stack_;
    };

    /**
     * @brief Wraps a captured call stack with an identifier suitable for transmission.
     */
    class Exception {
    public:
        Exception(std::unique_ptr<CallStack> callstack) : id_{exception_id_gen.fetch_add(1)}, callstack_(std::move(callstack)) {}
        //~Exception() = default;

        /**
         * @brief Returns the generated exception identifier.
         */
        int32_t getId() const { return id_; }
        /**
         * @brief Returns a reference to the captured call stack.
         */
        const CallStack& getCallStack() const { return *callstack_; }
        
        static std::atomic<int32_t> exception_id_gen;

    private:    
        int32_t id_;
        std::unique_ptr<CallStack> callstack_;
    };

} // namespace pinpoint
