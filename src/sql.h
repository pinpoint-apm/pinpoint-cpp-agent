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

namespace pinpoint {

    /**
    * SQL normalization result
    */
    struct SqlNormalizeResult {
        // Normalized SQL with numeric placeholders (1#, 2#, 3#...) and string placeholders ('1$', '2$', '3$'...)
        std::string normalized_sql;
        // Extracted numeric literals and string literals in order (comma-separated)
        std::string parameters;
        // Index of the next parameter to be added
        int param_index;

        SqlNormalizeResult() : normalized_sql(), parameters(), param_index(0) {}
    };

    /**
    * SQL Normalizer class for APM tracing
    * 
    * This class normalizes SQL queries by:
    * - Removing numeric literals and replacing with indexed placeholders (1#, 2#, 3#...)
    * - Removing string literals and replacing with indexed placeholders ('1$', '2$', '3$'...)
    * - Removing comments
    * - Extracting numeric literals and string literals in order (comma-separated)
    */
    class SqlNormalizer {
    public:
        /**
        * Constructor with optional configuration
        */
        explicit SqlNormalizer(size_t max_sql_length = 2048);
        
        /**
        * Destructor
        */
        ~SqlNormalizer() = default;
        
        /**
        * Normalize SQL query in one pass with numeric and string literal extraction
        * 
        * @param sql Raw SQL query string
        * @return SqlNormalizeResult with normalized SQL and extracted numeric/string literals (comma-separated)
        */
        SqlNormalizeResult normalize(std::string_view sql) const;

    private:
        size_t max_sql_length_;
        
        /**
        * Check if character is a quote character
        * 
        * @param c Character to check
        * @return True if quote character
        */
        static bool isQuoteChar(char c);

        /**
        * Handle string literal
        * 
        * @param sql SQL query string
        * @param sql_length Length of SQL to process
        * @param start_idx Index of the opening quote
        * @param quote_char Quote character
        * @param result Result of the normalization
        * @return Index of the next character to process
        */
        static size_t handleStringLiteral(std::string_view sql, size_t sql_length, size_t start_idx, char quote_char, SqlNormalizeResult& result);
        
        /**
        * Handle numeric literal
        * 
        * @param sql SQL query string
        * @param sql_length Length of SQL to process
        * @param start_idx Index of the starting position of the numeric literal
        * @param result Result of the normalization
        * @return Index of the last character of the numeric literal
        */
        static size_t handleNumericLiteral(std::string_view sql, size_t sql_length, size_t start_idx, SqlNormalizeResult& result);
    };
} // namespace pinpoint
