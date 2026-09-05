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

#include <string>
#include <string_view>
#include <vector>

namespace pinpoint {

    /**
    * SQL normalization result
    */
    struct SqlNormalizeResult {
        // Normalized SQL with numeric placeholders (0#, 1#, 2#...) and string placeholders ('0$', '1$', '2$'...)
        std::string normalized_sql;
        // Extracted numeric literals and string literals in order (comma-separated)
        std::string parameters;
        // Index of the next parameter to be added
        int param_index{0};
    };

    /**
    * @brief Hard memory cap on the SQL text the normalizer will process.
    *
    * Not a Java-parity value and not the metadata cap: like Java, the whole
    * statement is normalized and the result is the SQL id/UID cache key in
    * full, and only the copy that travels in PSqlMetaData.sql is abbreviated
    * (kMaxSqlMetaLength). This cap exists purely so a pathological
    * multi-megabyte statement cannot make one span allocate without bound.
    */
    inline constexpr size_t kMaxNormalizedSqlLength = 1024 * 1024;

    /**
    * SQL normalizer for APM tracing. Replaces numeric literals with indexed
    * placeholders (0#, 1#, ...) and string literals with '0$', '1$', ...,
    * optionally strips comments, and extracts both literal kinds in order
    * (comma-separated). Byte-compatible with the Java agent's
    * DefaultSqlNormalizer: the output is the SQL id/UID cache key.
    */
    class SqlNormalizer {
    public:
        /// Comments are removed by default, matching the Java agent
        /// (DefaultJdbcOption.removeComments=true). Nothing is put in their
        /// place — again like Java.
        explicit SqlNormalizer(size_t max_sql_length = 2048, bool remove_comments = true);
        ~SqlNormalizer() = default;

        /// Normalizes and extracts both literal kinds in a single pass.
        SqlNormalizeResult normalize(std::string_view sql) const;

    private:
        size_t max_sql_length_;
        bool remove_comments_;

        /// @p start_idx is the opening quote; returns the index of the next
        /// character to process.
        static size_t handleStringLiteral(std::string_view sql, size_t sql_length, size_t start_idx, char quote_char, SqlNormalizeResult& result);

        /// Returns the index of the literal's last character.
        static size_t handleNumericLiteral(std::string_view sql, size_t sql_length, size_t start_idx, SqlNormalizeResult& result);

        /**
        * Recomputes the "number token start enabled" flag after a regular
        * character, mirroring Java's ParserContext.numberTokenStartEnable. A
        * digit counts as a numeric literal only while the flag is enabled, so
        * a digit following an identifier character (the '1' in "col1") is left
        * alone. @p next_c covers the '$' positional-placeholder case, for which
        * @p current is returned unchanged when it is not a digit.
        */
        static bool updateNumberTokenStartEnable(char c, char next_c, bool current);

        static size_t readComment(std::string_view sql, size_t sql_length, size_t start_idx, std::string_view first_token,
            std::string_view end_token, std::string* writer);
    };
} // namespace pinpoint
