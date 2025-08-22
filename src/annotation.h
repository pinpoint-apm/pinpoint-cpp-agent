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

#include <list>
#include "pinpoint/tracer.h"

namespace pinpoint {

    typedef struct StringStringValue {
        std::string stringValue1;
        std::string stringValue2;

        StringStringValue(std::string_view strVal1, std::string_view strVal2)
            : stringValue1(strVal1), stringValue2(strVal2) {}
        ~StringStringValue() {}
    } StringStringValue;

    typedef struct IntStringStringValue {
        int intValue;
        std::string stringValue1;
        std::string stringValue2;

        IntStringStringValue(const int intVal, std::string_view strVal1, std::string_view strVal2)
            : intValue(intVal), stringValue1(strVal1), stringValue2(strVal2) {}
        ~IntStringStringValue() {}
    } IntStringStringValue;

    typedef struct LongIntIntByteByteStringValue {
        int64_t longValue;
        int32_t intValue1;
        int32_t intValue2;
        int32_t byteValue1;
        int32_t byteValue2;
        std::string stringValue;

        LongIntIntByteByteStringValue(const int64_t longVal, const int32_t intVal1, const int32_t intVal2, const int32_t byteVal1, const int32_t byteVal2, std::string_view strVal)
            : longValue(longVal), intValue1(intVal1), intValue2(intVal2),byteValue1(byteVal1), byteValue2(byteVal2), stringValue(strVal) {}
        ~LongIntIntByteByteStringValue() {}
    } LongIntIntByteByteStringValue;

    typedef union AnnotationValue {
        int intValue;
        std::string stringValue;
        StringStringValue stringStringValue;
        IntStringStringValue intStringStringValue;
        LongIntIntByteByteStringValue longIntIntByteByteStringValue;

        AnnotationValue(const int intVal) : intValue(intVal) {}
        AnnotationValue(std::string_view strVal) : stringValue(strVal) {}
        AnnotationValue(std::string_view strVal1, std::string_view strVal2) : stringStringValue(strVal1, strVal2) {}
        AnnotationValue(const int intVal, std::string_view strVal1, std::string_view strVal2)
            : intStringStringValue(intVal, strVal1, strVal2) {}
        AnnotationValue(const int64_t longVal, const int32_t intVal1, const int32_t intVal2, const int32_t byteVal1, const int32_t byteVal2, std::string_view strVal)
            : longIntIntByteByteStringValue(longVal, intVal1, intVal2, byteVal1, byteVal2, strVal) {}
        ~AnnotationValue() {}
    } AnnotationValue;

    typedef struct AnnotationData {
        int dataType;
        AnnotationValue data;

        AnnotationData(const int dType, const int intVal) : dataType(dType), data(intVal) {}
        AnnotationData(const int dType, std::string_view strVal) : dataType(dType), data(strVal) {}
        AnnotationData(const int dType, std::string_view strVal1, std::string_view strVal2) : dataType(dType), data(strVal1, strVal2) {}
        AnnotationData(const int dType, const int intVal, std::string_view strVal1, std::string_view strVal2)
            : dataType(dType), data(intVal, strVal1, strVal2) {}
        AnnotationData(const int dType, const int64_t longVal, const int32_t intVal1, const int32_t intVal2,
                       const int32_t byteVal1, const int32_t byteVal2, std::string_view strVal) : dataType(dType),
            data(longVal, intVal1, intVal2, byteVal1, byteVal2, strVal) {}
    } AnnotationData;

    class PinpointAnnotation final : public Annotation {
    public:
        PinpointAnnotation() {}
        ~PinpointAnnotation() override = default;

        void AppendInt(int32_t key, int i) override;
        void AppendString(int32_t key, std::string_view s) override;
        void AppendStringString(int32_t key, std::string_view s1, std::string_view s2) override;
        void AppendIntStringString(int32_t key, int i, std::string_view s1, std::string_view s2) override;
        void AppendLongIntIntByteByteString(int32_t key, int64_t l, int32_t i1, int32_t i2, int32_t b1, int32_t b2, std::string_view s) override;

        std::list<std::pair<int32_t,std::shared_ptr<AnnotationData>>>& getAnnotations() { return annotation_list_; }

    private:
        std::list<std::pair<int32_t,std::shared_ptr<AnnotationData>>> annotation_list_;
    };

} // namespace pinpoint
