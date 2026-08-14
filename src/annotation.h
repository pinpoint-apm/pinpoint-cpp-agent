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
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include "pinpoint/tracer.h"

namespace pinpoint {

    /// @brief Enumerates the supported annotation payload formats.
    enum AnnotationType {
        ANNOTATION_TYPE_INT = 0,
        ANNOTATION_TYPE_LONG = 1,
        ANNOTATION_TYPE_STRING = 2,
        ANNOTATION_TYPE_STRING_STRING = 3,
        ANNOTATION_TYPE_INT_STRING_STRING = 4,
        ANNOTATION_TYPE_LONG_INT_INT_BYTE_BYTE_STRING = 5,
        ANNOTATION_TYPE_BYTES_STRING_STRING = 6
    };

    /// @brief Container for annotations carrying an int and two strings.
    struct IntStringStringValue {
        int32_t intValue;
        std::string stringValue2;
        // The aliasing shared_ptr keeps the PreparedSql owner alive without
        // copying parameter bytes.
        std::shared_ptr<const std::string> sharedStringValue1;

        // strVal2 by value so a caller-owned bind-value string (see
        // SpanEventImpl::SetSqlQuery) moves in without copying up to
        // max_bind_args_size bytes; a literal still builds one std::string as
        // before.
        IntStringStringValue(const int32_t intVal,
                             std::shared_ptr<const std::string> strVal1,
                             std::string strVal2)
            : intValue(intVal), stringValue2(std::move(strVal2)),
              sharedStringValue1(std::move(strVal1)) {}

        std::string_view stringValue1View() const noexcept {
            return sharedStringValue1 ? std::string_view(*sharedStringValue1)
                                      : std::string_view();
        }
    };

    /// @brief Container for complex annotations that track timing and network details.
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

    /// @brief Container for annotations with binary payloads and additional strings.
    struct BytesStringStringValue {
        SqlUid bytesValue;
        std::string stringValue2;
        std::shared_ptr<const std::string> sharedStringValue1;

        // strVal2 by value; see IntStringStringValue above.
        BytesStringStringValue(SqlUid bytesVal,
                               std::shared_ptr<const std::string> strVal1,
                               std::string strVal2)
            : bytesValue(bytesVal), stringValue2(std::move(strVal2)),
              sharedStringValue1(std::move(strVal1)) {}

        std::string_view stringValue1View() const noexcept {
            return sharedStringValue1 ? std::string_view(*sharedStringValue1)
                                      : std::string_view();
        }
    };

    /**
     * @brief Type-safe variant wrapper for all supported annotation value types.
     *
     * The four payload shapes exposed by Span/SpanEvent::SetAnnotation come
     * first, followed by the internal-only formats.
     */
    using AnnotationDataValue = std::variant<
        int32_t,
        int64_t,
        std::string,
        std::pair<std::string, std::string>,
        IntStringStringValue,
        LongIntIntByteByteStringValue,
        BytesStringStringValue
    >;

    /// @brief Annotation payload whose type is derived from the stored value.
    struct AnnotationData {
        AnnotationDataValue data;

        explicit AnnotationData(const int32_t intVal)
            : data(intVal) {}
        explicit AnnotationData(const int64_t longVal)
            : data(longVal) {}
        explicit AnnotationData(std::string_view strVal)
            : data(std::string(strVal)) {}
        AnnotationData(std::string_view strVal1, std::string_view strVal2)
            : data(std::pair<std::string, std::string>(strVal1, strVal2)) {}
        AnnotationData(const int32_t intVal,
                       std::shared_ptr<const std::string> strVal1,
                       std::string strVal2)
            : data(IntStringStringValue(intVal, std::move(strVal1), std::move(strVal2))) {}
        AnnotationData(const int64_t longVal, const int32_t intVal1, const int32_t intVal2,
                       const int32_t byteVal1, const int32_t byteVal2, std::string_view strVal)
            : data(LongIntIntByteByteStringValue(longVal, intVal1, intVal2, byteVal1, byteVal2, strVal)) {}
        AnnotationData(SqlUid bytesVal,
                       std::shared_ptr<const std::string> strVal1,
                       std::string strVal2)
            : data(BytesStringStringValue(bytesVal, std::move(strVal1), std::move(strVal2))) {}

        AnnotationType type() const {
            // The variant alternatives are declared in exactly the enum's
            // order, so the variant discriminator is the annotation type.
            static_assert(std::is_same_v<std::variant_alternative_t<ANNOTATION_TYPE_INT, AnnotationDataValue>, int32_t>);
            static_assert(std::is_same_v<std::variant_alternative_t<ANNOTATION_TYPE_LONG, AnnotationDataValue>, int64_t>);
            static_assert(std::is_same_v<std::variant_alternative_t<ANNOTATION_TYPE_STRING, AnnotationDataValue>, std::string>);
            static_assert(std::is_same_v<std::variant_alternative_t<ANNOTATION_TYPE_STRING_STRING, AnnotationDataValue>, std::pair<std::string, std::string>>);
            static_assert(std::is_same_v<std::variant_alternative_t<ANNOTATION_TYPE_INT_STRING_STRING, AnnotationDataValue>, IntStringStringValue>);
            static_assert(std::is_same_v<std::variant_alternative_t<ANNOTATION_TYPE_LONG_INT_INT_BYTE_BYTE_STRING, AnnotationDataValue>, LongIntIntByteByteStringValue>);
            static_assert(std::is_same_v<std::variant_alternative_t<ANNOTATION_TYPE_BYTES_STRING_STRING, AnnotationDataValue>, BytesStringStringValue>);
            return static_cast<AnnotationType>(data.index());
        }
    };

    /**
     * @brief Concrete annotation container used by the Pinpoint agent.
     *
     * Accumulates annotation key/value pairs before they are serialized into
     * spans. Internal-only: user code records annotations through
     * Span/SpanEvent::SetAnnotation. Owned by value inside SpanData and
     * SpanEventImpl — an annotation-free owner pays no heap, since the list
     * below only allocates on the first append.
     */
    class PinpointAnnotation final {
    public:
        PinpointAnnotation() {}
        ~PinpointAnnotation() = default;

        /**
         * @brief Appends an integer value annotation.
         *
         * @param key Annotation identifier.
         * @param i Integer value to store.
         */
        void AppendInt(int32_t key, int32_t i);
        /**
         * @brief Appends a long value annotation.
         *
         * @param key Annotation identifier.
         * @param l Long value to store.
         */
        void AppendLong(int32_t key, int64_t l);
        /// @brief Appends an annotation containing two strings.
        void AppendStringString(int32_t key, std::string_view s1, std::string_view s2);
        /// @brief Appends a detailed network annotation used for RPC metadata
        ///        (@p l is typically the elapsed time).
        void AppendLongIntIntByteByteString(int32_t key, int64_t l, int32_t i1, int32_t i2, int32_t b1, int32_t b2, std::string_view s);

        /// @brief Appends a pre-built payload, letting internal callers move
        ///        large strings (e.g. normalized SQL parameters) in instead of
        ///        copying them through the string_view overloads.
        void AppendData(int32_t key, AnnotationData&& data);

        /**
         * @brief Permanently freezes the annotation list.
         *
         * Called by the owning span/span event when the list is handed to the
         * gRPC layer for serialization. Later Append calls become warn/no-ops:
         * an append path that bypasses the owner's finished guard (e.g. the
         * proxy-header recording through SpanData) would otherwise grow
         * annotation_list_ concurrently with the worker's iteration.
         */
        void seal() noexcept { sealed_.store(true, std::memory_order_release); }

        /**
         * @brief Releases the list's heap, leaving an empty (still sealed) list.
         *
         * Called by SpanEventImpl::releaseRetiredPayload() once the chunk is
         * done with the owning event. Destroying the elements also drops the
         * aliasing shared_ptr pins SQL annotations hold on PreparedSql cache
         * entries. Swap-with-temporary, not clear(): clear() keeps the capacity
         * allocated, and releasing that heap is the whole point.
         */
        void releaseStorage() noexcept {
            std::vector<std::pair<int32_t,AnnotationData>>{}.swap(annotation_list_);
        }

        /**
         * @brief Returns the internal annotation list for serialization.
         *
         * @return Reference to the stored annotations.
         */
        std::vector<std::pair<int32_t,AnnotationData>>& getAnnotations() { return annotation_list_; }
        /// @brief Const overload for read-only consumers (the gRPC serializer).
        const std::vector<std::pair<int32_t,AnnotationData>>& getAnnotations() const { return annotation_list_; }

    private:
        // Shared body of every Append overload: seal guard, initial reserve,
        // emplace, with allocation failures logged and swallowed. Defined in
        // annotation.cpp — all instantiations live there.
        template <typename... Args>
        void append(int32_t key, Args&&... args);

        // Most span events carry only a handful of annotations; reserving a
        // few slots up front skips the 1->2->4 growth reallocations.
        void reserveInitial() {
            if (annotation_list_.capacity() == 0) {
                annotation_list_.reserve(4);
            }
        }

        /// @brief Returns true (after logging a warning) once seal() was
        /// called, signalling that an Append must no-op. Mirrors the
        /// warnIfFinished guard on span events.
        bool warnIfSealed() const noexcept;

        std::vector<std::pair<int32_t,AnnotationData>> annotation_list_;
        std::atomic<bool> sealed_{false};
    };

} // namespace pinpoint
