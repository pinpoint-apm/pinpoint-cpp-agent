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
        if (sql.empty()) {
            return SqlNormalizeResult("", "");
        }
        
        // Limit SQL length to prevent memory issues
        const size_t sql_length = std::min(sql.length(), max_sql_length_);
        
        std::string normalized_sql;
        normalized_sql.reserve(sql_length);
        
        std::string parameters;
        // Reserve some initial space for parameters to avoid frequent reallocations
        parameters.reserve(64);
        
        enum class State {
            Normal,
            InLineComment,
            InBlockComment,
            InBlockCommentEnd
        };
        
        State state = State::Normal;
        int param_index = 0;
        
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
                        char quote_char = c;
                        size_t start_idx = i + 1; // Start of content (after opening quote)
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
                            
                            for (size_t k = start_idx; k < current_idx; ++k) {
                                if (sql[k] == quote_char && k + 1 < current_idx && sql[k + 1] == quote_char) {
                                    content += quote_char;
                                    k++; // Skip escaped char
                                } else {
                                    content += sql[k];
                                }
                            }

                            // Add to parameters
                            if (param_index > 0) {
                                parameters += ',';
                            }
                            parameters += content;

                            // Add placeholder to SQL
                            normalized_sql += quote_char;
                            normalized_sql += std::to_string(param_index);
                            normalized_sql += '$';
                            normalized_sql += quote_char;
                            
                            param_index++;
                            i = current_idx; // Move main loop index to closing quote
                        } else {
                            // Malformed string - include as is from the opening quote
                            // We only add the opening quote here, the rest will be processed in next iterations
                            // But waiting... if we just add 'c', the loop continues. 
                            // The original logic consumed the whole string.
                            // Let's consume it all as is.
                            normalized_sql += quote_char;
                            for (size_t k = start_idx; k < sql_length; ++k) {
                                normalized_sql += sql[k];
                            }
                            i = sql_length; // End loop
                        }
                        
                        continue;
                    }
                    
                    
                    // Handle numeric literals
                    // Check for digit, or minus sign followed by digit
                    if (std::isdigit(static_cast<unsigned char>(c)) || (c == '-' && next_c != '\0' && std::isdigit(static_cast<unsigned char>(next_c)))) {
                        std::string number;
                        
                        size_t num_start = i;
                        if (c == '-') {
                            i++;
                        }
                        
                        // Read the entire number (including decimals)
                        while (i < sql_length) {
                            char nc = sql[i];
                            if (std::isdigit(static_cast<unsigned char>(nc)) || nc == '.') {
                                i++;
                            } else {
                                break;
                            }
                        }
                        
                        // Extract number string
                        number.assign(sql.data() + num_start, i - num_start);
                        
                        // Add to parameters
                        if (param_index > 0) {
                            parameters += ',';
                        }
                        parameters += number;

                        // Add placeholder
                        normalized_sql += std::to_string(param_index);
                        normalized_sql += '#';
                        
                        param_index++;
                        i--; // Back up one since the loop will increment
                        continue;
                    }
                    
                    // Regular character
                    normalized_sql += c;
                    break;
                }
                
                case State::InLineComment: {
                    if (c == '\n' || c == '\r') {
                        state = State::Normal;
                        normalized_sql += c;
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
                    normalized_sql += ' ';
                    break;
                }
            }
        }
        
        return SqlNormalizeResult(std::move(normalized_sql), std::move(parameters));
    }

    bool SqlNormalizer::isQuoteChar(char c) {
        return c == '\'' || c == '"' || c == '`';
    }

} // namespace pinpoint
