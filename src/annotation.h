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

#include <type_traits>
#include <variant>
#include <vector>
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
    struct StringStringValue {
        std::string stringValue1;
        std::string stringValue2;

        StringStringValue(std::string_view strVal1, std::string_view strVal2)
            : stringValue1(strVal1), stringValue2(strVal2) {}
    };

    /**
     * @brief Container for annotations carrying an int and two strings.
     */
    struct IntStringStringValue {
        int intValue;
        std::string stringValue1;
        std::string stringValue2;

        IntStringStringValue(const int intVal, std::string_view strVal1, std::string_view strVal2)
            : intValue(intVal), stringValue1(strVal1), stringValue2(strVal2) {}
        IntStringStringValue(const int intVal, std::string&& strVal1, std::string_view strVal2)
            : intValue(intVal), stringValue1(std::move(strVal1)), stringValue2(strVal2) {}
    };

    /**
     * @brief Container for complex annotations that track timing and network details.
     */
    struct LongIntIntByteByteStringValue {
        int64_t longValue;
        int32_t intValue1;
        int32_t intValue2;
        int32_t byteValue1;
        int32_t byteValue2;
        std::string stringValue;

        LongIntIntByteByteStringValue(const int64_t longVal, const int32_t intVal1, const int32_t intVal2, const int32_t byteVal1, const int32_t byteVal2, std::string_view strVal)
            : longValue(longVal), intValue1(intVal1), intValue2(intVal2),byteValue1(byteVal1), byteValue2(byteVal2), stringValue(strVal) {}
    };

    /**
     * @brief Container for annotations with binary payloads and additional strings.
     */
    struct BytesStringStringValue {
        SqlUid bytesValue;
        std::string stringValue1;
        std::string stringValue2;

        BytesStringStringValue(SqlUid bytesVal, std::string_view strVal1, std::string_view strVal2)
            : bytesValue(bytesVal), stringValue1(strVal1), stringValue2(strVal2) {}
        BytesStringStringValue(SqlUid bytesVal, std::string&& strVal1, std::string_view strVal2)
            : bytesValue(bytesVal), stringValue1(std::move(strVal1)), stringValue2(strVal2) {}
    };

    /**
     * @brief Type-safe variant wrapper for all supported annotation value types.
     */
    using AnnotationValue = std::variant<
        int32_t,
        int64_t,
        std::string,
        StringStringValue,
        IntStringStringValue,
        LongIntIntByteByteStringValue,
        BytesStringStringValue
    >;

    /**
     * @brief Annotation payload whose type is derived from the stored value.
     */
    struct AnnotationData {
        AnnotationValue data;

        explicit AnnotationData(const int32_t intVal)
            : data(intVal) {}
        explicit AnnotationData(const int64_t longVal)
            : data(longVal) {}
        explicit AnnotationData(std::string_view strVal)
            : data(std::string(strVal)) {}
        AnnotationData(std::string_view strVal1, std::string_view strVal2)
            : data(StringStringValue(strVal1, strVal2)) {}
        AnnotationData(const int intVal, std::string_view strVal1, std::string_view strVal2)
            : data(IntStringStringValue(intVal, strVal1, strVal2)) {}
        AnnotationData(const int intVal, std::string&& strVal1, std::string_view strVal2)
            : data(IntStringStringValue(intVal, std::move(strVal1), strVal2)) {}
        AnnotationData(const int64_t longVal, const int32_t intVal1, const int32_t intVal2,
                       const int32_t byteVal1, const int32_t byteVal2, std::string_view strVal)
            : data(LongIntIntByteByteStringValue(longVal, intVal1, intVal2, byteVal1, byteVal2, strVal)) {}
        AnnotationData(SqlUid bytesVal, std::string_view strVal1, std::string_view strVal2)
            : data(BytesStringStringValue(bytesVal, strVal1, strVal2)) {}
        AnnotationData(SqlUid bytesVal, std::string&& strVal1, std::string_view strVal2)
            : data(BytesStringStringValue(bytesVal, std::move(strVal1), strVal2)) {}

        AnnotationType type() const {
            // The variant alternatives are declared in exactly the enum's
            // order, so the variant discriminator is the annotation type.
            static_assert(std::is_same_v<std::variant_alternative_t<ANNOTATION_TYPE_INT, AnnotationValue>, int32_t>);
            static_assert(std::is_same_v<std::variant_alternative_t<ANNOTATION_TYPE_LONG, AnnotationValue>, int64_t>);
            static_assert(std::is_same_v<std::variant_alternative_t<ANNOTATION_TYPE_STRING, AnnotationValue>, std::string>);
            static_assert(std::is_same_v<std::variant_alternative_t<ANNOTATION_TYPE_STRING_STRING, AnnotationValue>, StringStringValue>);
            static_assert(std::is_same_v<std::variant_alternative_t<ANNOTATION_TYPE_INT_STRING_STRING, AnnotationValue>, IntStringStringValue>);
            static_assert(std::is_same_v<std::variant_alternative_t<ANNOTATION_TYPE_LONG_INT_INT_BYTE_BYTE_STRING, AnnotationValue>, LongIntIntByteByteStringValue>);
            static_assert(std::is_same_v<std::variant_alternative_t<ANNOTATION_TYPE_BYTES_STRING_STRING, AnnotationValue>, BytesStringStringValue>);
            return static_cast<AnnotationType>(data.index());
        }
    };

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
         * @brief Appends an annotation containing a SQL UID and two strings.
         *
         * @param key Annotation identifier.
         * @param uid 16-byte SQL UID payload.
         * @param s1 First string.
         * @param s2 Second string.
         */
        void AppendSqlUidStringString(int32_t key, SqlUid uid, std::string_view s1, std::string_view s2) override;
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
         * @brief Appends a pre-built annotation payload.
         *
         * Lets internal callers move large strings (e.g. normalized SQL
         * parameters) into the annotation instead of copying them through
         * the string_view Append overloads.
         *
         * @param key Annotation identifier.
         * @param data Annotation payload (moved).
         */
        void AppendData(int32_t key, AnnotationData&& data);

        /**
         * @brief Returns the internal annotation list for serialization.
         *
         * @return Reference to the stored annotations.
         */
        std::vector<std::pair<int32_t,AnnotationData>>& getAnnotations() { return annotation_list_; }

    private:
        // Most span events carry only a handful of annotations; reserving a
        // few slots up front skips the 1->2->4 growth reallocations.
        void reserveInitial() {
            if (annotation_list_.capacity() == 0) {
                annotation_list_.reserve(4);
            }
        }

        std::vector<std::pair<int32_t,AnnotationData>> annotation_list_;
    };

} // namespace pinpoint
