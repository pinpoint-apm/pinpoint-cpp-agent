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

#include "sql.h"
#include <algorithm>
#include <cctype>

namespace pinpoint {

    SqlNormalizer::SqlNormalizer(size_t max_sql_length) 
        : max_sql_length_(max_sql_length) {
    }

    SqlNormalizeResult SqlNormalizer::normalize(std::string_view sql) const {
        auto result = SqlNormalizeResult();
        if (sql.empty()) {
            return result;
        }
        
        // Limit SQL length to prevent memory issues
        const size_t sql_length = std::min(sql.length(), max_sql_length_);
        result.normalized_sql.reserve(sql_length);
        result.parameters.reserve(64);
        
        enum class State {
            Normal,
            InLineComment,
            InBlockComment,
            InBlockCommentEnd
        };
        
        State state = State::Normal;
        
        for (size_t i = 0; i < sql_length; ++i) {
            char c = sql[i];
            char next_c = (i + 1 < sql_length) ? sql[i + 1] : '\0';
            
            switch (state) {
                case State::Normal: {
                    // Handle start of comments
                    if (c == '-' && next_c == '-') {
                        state = State::InLineComment;
                        i++; // Skip next '-'
                        continue;
                    }
                    if (c == '/' && next_c == '*') {
                        state = State::InBlockComment;
                        i++; // Skip next '*'
                        continue;
                    }
                    
                    // Handle start of string literals
                    if (isQuoteChar(c)) {
                        i = handleStringLiteral(sql, sql_length, i + 1, c, result);
                        continue;
                    }
                    
                    // Handle numeric literals
                    // Check for digit, or minus sign followed by digit
                    if (std::isdigit(static_cast<unsigned char>(c)) 
                        || (c == '-' && next_c != '\0' && std::isdigit(static_cast<unsigned char>(next_c)))) {
                        i = handleNumericLiteral(sql, sql_length, i, result);
                        continue;
                    }
                    
                    // Regular character
                    result.normalized_sql += c;
                    break;
                }
                
                case State::InLineComment: {
                    if (c == '\n' || c == '\r') {
                        state = State::Normal;
                        result.normalized_sql += c;
                    }
                    // Skip all other characters in line comment
                    break;
                }
                
                case State::InBlockComment: {
                    if (c == '*' && next_c == '/') {
                        state = State::InBlockCommentEnd;
                    }
                    // Skip all characters in block comment
                    break;
                }
                
                case State::InBlockCommentEnd: {
                    state = State::Normal;
                    // Add space to replace the comment
                    result.normalized_sql += ' ';
                    break;
                }
            }
        }
        
        return result;
    }

    bool SqlNormalizer::isQuoteChar(char c) {
        return c == '\'' || c == '"' || c == '`';
    }

    size_t SqlNormalizer::handleStringLiteral(std::string_view sql, size_t sql_length, size_t start_idx, char quote_char, SqlNormalizeResult& result) {
        size_t current_idx = start_idx;
        bool closed = false;
        
        // Scan for closing quote
        while (current_idx < sql_length) {
            if (sql[current_idx] == quote_char) {
                // Check for escaped quote (e.g. 'don''t')
                if (current_idx + 1 < sql_length && sql[current_idx + 1] == quote_char) {
                    current_idx += 2; // Skip both quotes
                } else {
                    closed = true;
                    break;
                }
            } else {
                current_idx++;
            }
        }

        if (closed) {
            // Extract content, handling escaped quotes if necessary
            std::string content;
            size_t content_len = current_idx - start_idx;
            content.reserve(content_len);
            
            for (size_t i = start_idx; i < current_idx; ++i) {
                if (sql[i] == quote_char && i + 1 < current_idx && sql[i + 1] == quote_char) {
                    content += quote_char;
                    i++; // Skip escaped char
                } else {
                    content += sql[i];
                }
            }

            // Add to parameters
            if (result.param_index > 0) {
                result.parameters += ',';
            }
            result.parameters += content;

            // Add placeholder to SQL
            result.normalized_sql += quote_char;
            result.normalized_sql += std::to_string(result.param_index);
            result.normalized_sql += '$';
            result.normalized_sql += quote_char;
            
            result.param_index++;
        } else {
            // Malformed string - include as is from the opening quote
            // We only add the opening quote here, the rest will be processed in next iterations
            // But waiting... if we just add 'quote_char', the loop continues. 
            // The original logic consumed the whole string.
            // Let's consume it all as is.
            result.normalized_sql += quote_char;
            for (size_t i = start_idx; i < sql_length; ++i) {
                result.normalized_sql += sql[i];
            }
            current_idx = sql_length; // End loop
        }

        return current_idx;
    }

    size_t SqlNormalizer::handleNumericLiteral(std::string_view sql, size_t sql_length, size_t start_idx, SqlNormalizeResult& result) {
        size_t current_idx = start_idx;
        
        if (sql[current_idx] == '-') {
            current_idx++;
        }
        
        // Read the entire number (including decimals)
        while (current_idx < sql_length) {
            char nc = sql[current_idx];
            if (std::isdigit(static_cast<unsigned char>(nc)) || nc == '.') {
                current_idx++;
            } else {
                break;
            }
        }
        
        // Extract number string
        std::string number;
        number.assign(sql.data() + start_idx, current_idx - start_idx);
        
        // Add to parameters
        if (result.param_index > 0) {
            result.parameters += ',';
        }
        result.parameters += number;

        // Add placeholder
        result.normalized_sql += std::to_string(result.param_index);
        result.normalized_sql += '#';
        
        result.param_index++;
        return current_idx - 1; // Back up one since the loop will increment
    }

} // namespace pinpoint
