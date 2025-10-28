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

#include <vector>
#include <string>
#include <chrono>
#include "utility.h"

namespace pinpoint {
 
    typedef struct {
        std::string module;
        std::string function;
        std::string file;
        int line;
    } StackFrame;
 
    class CallStack {
    public:
        CallStack(std::string_view error_message) : error_message_(error_message),
                                               error_time_{to_milli_seconds(std::chrono::system_clock::now())},
                                               stack_{} {}
        //~CallStack() = default;

        void push(std::string_view module, std::string_view function, std::string_view file, int line) {
            stack_.emplace_back(StackFrame{module.data(), function.data(), file.data(), line});
        }

        std::string& getErrorMessage() {
            return error_message_;
        }

        int64_t getErrorTime() const {
            return error_time_;
        }

        std::vector<StackFrame>& getStack() {
            return stack_;
        }

        std::string& getModuleName() {
            return stack_[0].module;
        }

    private:
        std::string error_message_;
        int64_t error_time_;
        std::vector<StackFrame> stack_;
    };

    class Exception {
    public:
        Exception(std::unique_ptr<CallStack> callstack) : id_{exception_id_gen.fetch_add(1)}, callstack_(std::move(callstack)) {}
        //~Exception() = default;

        int32_t getId() const { return id_; }
        std::unique_ptr<CallStack> getCallStack() { return std::move(callstack_); }
        
        static std::atomic<int32_t> exception_id_gen;

    private:    
        int32_t id_;
        std::unique_ptr<CallStack> callstack_;
    };

} // namespace pinpoint
