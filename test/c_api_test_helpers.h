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

#include <map>
#include <string>

#include "pinpoint/tracer_c.h"

namespace pinpoint::test::c_api {

using HeaderMap = std::map<std::string, std::string>;

extern "C" inline const char* map_get(void* userdata, const char* key) {
    const auto& values = *static_cast<HeaderMap*>(userdata);
    const auto it = values.find(key);
    return it == values.end() ? nullptr : it->second.c_str();
}

extern "C" inline void map_set(void* userdata, const char* key,
                               const char* value) {
    (*static_cast<HeaderMap*>(userdata))[key] = value;
}

extern "C" inline void map_for_each(void* userdata,
                                    pt_header_foreach_cb callback,
                                    void* callback_userdata) {
    for (const auto& [key, value] : *static_cast<HeaderMap*>(userdata)) {
        if (callback(key.c_str(), value.c_str(), callback_userdata) != 0) {
            break;
        }
    }
}

struct CallStackFrame {
    const char* module;
    const char* function;
    const char* file;
    int line;
};

struct TwoFrameCallStack {
    CallStackFrame frames[2];
};

extern "C" inline void emit_two_frame_callstack(
    void* userdata, pt_callstack_frame_cb callback,
    void* callback_userdata) {
    const auto& callstack = *static_cast<TwoFrameCallStack*>(userdata);
    for (const auto& frame : callstack.frames) {
        callback(frame.module, frame.function, frame.file, frame.line,
                 callback_userdata);
    }
}

}  // namespace pinpoint::test::c_api
