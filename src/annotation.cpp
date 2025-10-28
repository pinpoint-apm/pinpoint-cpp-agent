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

    void PinpointAnnotation::AppendInt(int32_t key, int32_t i) {
        try {
            auto a = std::make_shared<AnnotationData>(ANNOTATION_TYPE_INT, i);
            annotation_list_.emplace_back(key, a);
        } catch (const std::exception& e) {
            LOG_ERROR("make annotation data exception = {}", e.what());
        }
    }

    void PinpointAnnotation::AppendLong(int32_t key, int64_t l) {
        try {
            auto a = std::make_shared<AnnotationData>(ANNOTATION_TYPE_LONG, l);
            annotation_list_.emplace_back(key, a);
        } catch (const std::exception& e) {
            LOG_ERROR("make annotation data exception = {}", e.what());
        }
    }
    void PinpointAnnotation::AppendString(int32_t key, std::string_view s) {
        try {
            auto a = std::make_shared<AnnotationData>(ANNOTATION_TYPE_STRING, s);
            annotation_list_.emplace_back(key, a);
        } catch (const std::exception& e) {
            LOG_ERROR("make annotation data exception = {}", e.what());
        }
    }

    void PinpointAnnotation::AppendStringString(int32_t key, std::string_view s1, std::string_view s2) {
        try {
            auto a = std::make_shared<AnnotationData>(ANNOTATION_TYPE_STRING_STRING, s1, s2);
            annotation_list_.emplace_back(key, a);
        } catch (const std::exception& e) {
            LOG_ERROR("make annotation data exception = {}", e.what());
        }
    }

    void PinpointAnnotation::AppendIntStringString(int32_t key, int i, std::string_view s1, std::string_view s2) {
        try {
            auto a = std::make_shared<AnnotationData>(ANNOTATION_TYPE_INT_STRING_STRING, i, s1, s2);
            annotation_list_.emplace_back(key, a);
        } catch (const std::exception& e) {
            LOG_ERROR("make annotation data exception = {}", e.what());
        }
    }

    void PinpointAnnotation::AppendBytesStringString(int32_t key, std::vector<unsigned char> uid, std::string_view s1, std::string_view s2) {
        try {
            auto a = std::make_shared<AnnotationData>(ANNOTATION_TYPE_BYTES_STRING_STRING, std::move(uid), s1, s2);
            annotation_list_.emplace_back(key, a);
        } catch (const std::exception& e) {
            LOG_ERROR("make annotation data exception = {}", e.what());
        }
    }

    void PinpointAnnotation::AppendLongIntIntByteByteString(int32_t key, int64_t l, int32_t i1, int32_t i2, int32_t b1,
                                                        int32_t b2, std::string_view s) {
        try {
            auto a = std::make_shared<AnnotationData>(ANNOTATION_TYPE_LONG_INT_INT_BYTE_BYTE_STRING, l, i1, i2, b1, b2, s);
            annotation_list_.emplace_back(key, a);
        } catch (const std::exception& e) {
            LOG_ERROR("make annotation data exception = {}", e.what());
        }
    }
} // namespace pinpoint
