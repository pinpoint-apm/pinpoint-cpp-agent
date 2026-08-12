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
#include "annotation.h"

namespace pinpoint {

    namespace {
        // The realistic cause of the exception being handled is memory
        // pressure, and log formatting allocates too: swallow a logging
        // failure instead of letting it escape into user code.
        void logAppendFailure(const std::exception& e) noexcept {
            try {
                LOG_ERROR("make annotation data exception = {}", e.what());
            } catch (...) {
            }
        }
    }

    bool PinpointAnnotation::warnIfSealed() const noexcept {
        if (!sealed_.load(std::memory_order_acquire)) {
            return false;
        }
        try {
            LOG_WARN("annotation is already finished");
        } catch (...) {
        }
        return true;
    }

    template <typename... Args>
    void PinpointAnnotation::append(int32_t key, Args&&... args) {
        if (warnIfSealed()) return;
        try {
            reserveInitial();
            annotation_list_.emplace_back(key, AnnotationData(std::forward<Args>(args)...));
        } catch (const std::exception& e) {
            logAppendFailure(e);
        }
    }

    void PinpointAnnotation::AppendInt(int32_t key, int32_t i) { append(key, i); }

    void PinpointAnnotation::AppendLong(int32_t key, int64_t l) { append(key, l); }

    void PinpointAnnotation::AppendStringString(int32_t key, std::string_view s1, std::string_view s2) {
        append(key, s1, s2);
    }

    void PinpointAnnotation::AppendLongIntIntByteByteString(int32_t key, int64_t l, int32_t i1, int32_t i2, int32_t b1,
                                                        int32_t b2, std::string_view s) {
        append(key, l, i1, i2, b1, b2, s);
    }

    void PinpointAnnotation::AppendData(int32_t key, AnnotationData&& data) { append(key, std::move(data)); }
} // namespace pinpoint
