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
    std::string normalized_sql;  // Normalized SQL with numeric placeholders (1#, 2#, 3#...) and string placeholders ('1$', '2$', '3$'...)
    std::string parameters;      // Extracted numeric literals and string literals in order (comma-separated)
    
    SqlNormalizeResult() = default;
    SqlNormalizeResult(std::string sql, std::string params) 
        : normalized_sql(std::move(sql)), parameters(std::move(params)) {}
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
    
};

} // namespace pinpoint
