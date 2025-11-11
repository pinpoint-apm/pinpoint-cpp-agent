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
        CallStack(std::string_view error_message) : error_message_(error_message),
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
            stack_.emplace_back(StackFrame{module.data(), function.data(), file.data(), line});
        }

        /**
         * @brief Returns the recorded error message.
         *
         * @return Mutable reference to the message.
         */
        std::string& getErrorMessage() {
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
        std::vector<StackFrame>& getStack() {
            return stack_;
        }

        /**
         * @brief Convenience accessor for the module name of the top frame.
         */
        std::string& getModuleName() {
            return stack_[0].module;
        }

    private:
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
         * @brief Transfers ownership of the captured call stack.
         */
        std::unique_ptr<CallStack> getCallStack() { return std::move(callstack_); }
        
        static std::atomic<int32_t> exception_id_gen;

    private:    
        int32_t id_;
        std::unique_ptr<CallStack> callstack_;
    };

} // namespace pinpoint
