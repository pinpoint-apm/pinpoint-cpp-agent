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

    /**
     * @brief Enumerates the supported annotation payload formats.
     */
    enum AnnotationType {
        ANNOTATION_TYPE_INT = 0,
        ANNOTATION_TYPE_LONG = 1,
        ANNOTATION_TYPE_STRING = 2,
        ANNOTATION_TYPE_STRING_STRING = 3,
        ANNOTATION_TYPE_INT_STRING_STRING = 4,
        ANNOTATION_TYPE_LONG_INT_INT_BYTE_BYTE_STRING = 5,
        ANNOTATION_TYPE_BYTES_STRING_STRING = 6
    };

    /**
     * @brief Container for annotations composed of two string values.
     */
    typedef struct StringStringValue {
        std::string stringValue1;
        std::string stringValue2;

        StringStringValue(std::string_view strVal1, std::string_view strVal2)
            : stringValue1(strVal1), stringValue2(strVal2) {}
        ~StringStringValue() {}
    } StringStringValue;

    /**
     * @brief Container for annotations carrying an int and two strings.
     */
    typedef struct IntStringStringValue {
        int intValue;
        std::string stringValue1;
        std::string stringValue2;

        IntStringStringValue(const int intVal, std::string_view strVal1, std::string_view strVal2)
            : intValue(intVal), stringValue1(strVal1), stringValue2(strVal2) {}
        ~IntStringStringValue() {}
    } IntStringStringValue;

    /**
     * @brief Container for complex annotations that track timing and network details.
     */
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

    /**
     * @brief Container for annotations with binary payloads and additional strings.
     */
    typedef struct BytesStringStringValue {
        std::vector<unsigned char> bytesValue;
        std::string stringValue1;
        std::string stringValue2;

        BytesStringStringValue(std::vector<unsigned char> bytesVal, std::string_view strVal1, std::string_view strVal2)
            : bytesValue(std::move(bytesVal)), stringValue1(strVal1), stringValue2(strVal2) {}
        ~BytesStringStringValue() {}
    } BytesStringStringValue;

    /**
     * @brief Union wrapper for all supported annotation value variants.
     */
    typedef union AnnotationValue {
        int32_t intValue;
        int64_t longValue;
        std::string stringValue;
        StringStringValue stringStringValue;
        IntStringStringValue intStringStringValue;
        LongIntIntByteByteStringValue longIntIntByteByteStringValue;
        BytesStringStringValue bytesStringStringValue;

        AnnotationValue(const int32_t intVal) : intValue(intVal) {}
        AnnotationValue(const int64_t longVal) : longValue(longVal) {}
        AnnotationValue(std::string_view strVal) : stringValue(strVal) {}
        AnnotationValue(std::string_view strVal1, std::string_view strVal2) : stringStringValue(strVal1, strVal2) {}
        AnnotationValue(const int intVal, std::string_view strVal1, std::string_view strVal2)
            : intStringStringValue(intVal, strVal1, strVal2) {}
        AnnotationValue(const int64_t longVal, const int32_t intVal1, const int32_t intVal2, const int32_t byteVal1, const int32_t byteVal2, std::string_view strVal)
            : longIntIntByteByteStringValue(longVal, intVal1, intVal2, byteVal1, byteVal2, strVal) {}
        AnnotationValue(std::vector<unsigned char> bytesVal, std::string_view strVal1, std::string_view strVal2)
            : bytesStringStringValue(std::move(bytesVal), strVal1, strVal2) {}
        ~AnnotationValue() {}
    } AnnotationValue;

    /**
     * @brief Annotation payload paired with its declared type.
     */
    typedef struct AnnotationData {
        AnnotationType dataType;
        AnnotationValue data;

        AnnotationData(const AnnotationType dType, const int32_t intVal) : dataType(dType), data(intVal) {}
        AnnotationData(const AnnotationType dType, const int64_t longVal) : dataType(dType), data(longVal) {}
        AnnotationData(const AnnotationType dType, std::string_view strVal) : dataType(dType), data(strVal) {}
        AnnotationData(const AnnotationType dType, std::string_view strVal1, std::string_view strVal2) : dataType(dType), data(strVal1, strVal2) {}
        AnnotationData(const AnnotationType dType, const int intVal, std::string_view strVal1, std::string_view strVal2)
            : dataType(dType), data(intVal, strVal1, strVal2) {}
        AnnotationData(const AnnotationType dType, const int64_t longVal, const int32_t intVal1, const int32_t intVal2,
                       const int32_t byteVal1, const int32_t byteVal2, std::string_view strVal) : dataType(dType),
            data(longVal, intVal1, intVal2, byteVal1, byteVal2, strVal) {}
        AnnotationData(const AnnotationType dType, std::vector<unsigned char> bytesVal, std::string_view strVal1, std::string_view strVal2)
            : dataType(dType), data(std::move(bytesVal), strVal1, strVal2) {}
    } AnnotationData;

    /**
     * @brief Concrete annotation implementation used by the Pinpoint agent.
     *
     * Accumulates annotation key/value pairs before they are serialized into spans.
     */
    class PinpointAnnotation final : public Annotation {
    public:
        PinpointAnnotation() {}
        ~PinpointAnnotation() override = default;

        /**
         * @brief Appends an integer value annotation.
         *
         * @param key Annotation identifier.
         * @param i Integer value to store.
         */
        void AppendInt(int32_t key, int32_t i) override;
        /**
         * @brief Appends a long value annotation.
         *
         * @param key Annotation identifier.
         * @param l Long value to store.
         */
        void AppendLong(int32_t key, int64_t l) override;
        /**
         * @brief Appends a string value annotation.
         *
         * @param key Annotation identifier.
         * @param s String value to store.
         */
        void AppendString(int32_t key, std::string_view s) override;
        /**
         * @brief Appends an annotation containing two strings.
         *
         * @param key Annotation identifier.
         * @param s1 First string.
         * @param s2 Second string.
         */
        void AppendStringString(int32_t key, std::string_view s1, std::string_view s2) override;
        /**
         * @brief Appends an annotation containing an integer and two strings.
         *
         * @param key Annotation identifier.
         * @param i Integer payload.
         * @param s1 First string.
         * @param s2 Second string.
         */
        void AppendIntStringString(int32_t key, int i, std::string_view s1, std::string_view s2) override;
        /**
         * @brief Appends an annotation containing binary data and two strings.
         *
         * @param key Annotation identifier.
         * @param uid Binary payload.
         * @param s1 First string.
         * @param s2 Second string.
         */
        void AppendBytesStringString(int32_t key, std::vector<unsigned char> uid, std::string_view s1, std::string_view s2) override;
        /**
         * @brief Appends a detailed network annotation used for RPC metadata.
         *
         * @param key Annotation identifier.
         * @param l Long payload (typically elapsed time).
         * @param i1 First integer payload.
         * @param i2 Second integer payload.
         * @param b1 First byte payload.
         * @param b2 Second byte payload.
         * @param s String payload.
         */
        void AppendLongIntIntByteByteString(int32_t key, int64_t l, int32_t i1, int32_t i2, int32_t b1, int32_t b2, std::string_view s) override;

        /**
         * @brief Returns the internal annotation map for serialization.
         *
         * @return Reference to the stored annotations.
         */
        std::list<std::pair<int32_t,std::shared_ptr<AnnotationData>>>& getAnnotations() { return annotation_list_; }

    private:
        std::list<std::pair<int32_t,std::shared_ptr<AnnotationData>>> annotation_list_;
    };

} // namespace pinpoint
