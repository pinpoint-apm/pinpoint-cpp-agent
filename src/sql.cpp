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
#include <sstream>
#include <vector>

namespace pinpoint {

    SqlNormalizer::SqlNormalizer(size_t max_sql_length) 
        : max_sql_length_(max_sql_length) {
    }

    SqlNormalizeResult SqlNormalizer::normalize(std::string_view sql) const {
        if (sql.empty()) {
            return SqlNormalizeResult("", "");
        }
        
        // Limit SQL length to prevent memory issues
        std::string limited_sql(sql.substr(0, std::min(sql.length(), max_sql_length_)));
        
        std::ostringstream result_stream;
        std::vector<std::string> parameters;
        
        enum class State {
            Normal,
            InLineComment,
            InBlockComment,
            InBlockCommentEnd
        };
        
        State state = State::Normal;
        size_t param_index = 1;
        const size_t sql_length = limited_sql.length();
        
        for (size_t i = 0; i < sql_length; ++i) {
            char c = limited_sql[i];
            char next_c = (i + 1 < sql_length) ? limited_sql[i + 1] : '\0';
            
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
                    
                    // Handle start of string literals - extract and replace with indexed placeholders
                    if (c == '\'' || c == '"' || c == '`') {
                        std::string string_literal;
                        char quote_char = c;
                        string_literal += c; // Include opening quote
                        i++; // Skip opening quote
                        
                        // Read the entire string literal including closing quote
                        bool closed = false;
                        while (i < sql_length) {
                            char str_c = limited_sql[i];
                            string_literal += str_c;
                            
                            if (str_c == quote_char) {
                                // Check for escaped quote
                                if (i + 1 < sql_length && limited_sql[i + 1] == quote_char) {
                                    i++; // Skip the escaped quote
                                    string_literal += limited_sql[i];
                                } else {
                                    closed = true;
                                    break;
                                }
                            } else if (str_c == '\\' && i + 1 < sql_length) {
                                // Handle backslash escapes
                                i++;
                                string_literal += limited_sql[i];
                            }
                            i++;
                        }
                        
                        if (closed) {
                            // Add to parameters and replace with indexed placeholder
                            parameters.push_back(string_literal);
                            result_stream << quote_char << param_index << '$' << quote_char;
                            param_index++;
                        } else {
                            // Malformed string - include as is
                            result_stream << string_literal;
                        }
                        
                        continue;
                    }
                    
                    
                    // Handle numeric literals
                    if (std::isdigit(c) || (c == '-' && next_c != '\0' && std::isdigit(next_c))) {
                        std::string number;
                        
                        if (c == '-') {
                            number += c;
                            i++;
                            c = limited_sql[i];
                        }
                        
                        // Read the entire number (including decimals)
                        while (i < sql_length && (std::isdigit(limited_sql[i]) || limited_sql[i] == '.')) {
                            number += limited_sql[i];
                            i++;
                        }
                        i--; // Back up one since the loop will increment
                        
                        // Add to parameters and replace with indexed placeholder
                        parameters.push_back(number);
                        result_stream << param_index << '#';
                        param_index++;
                        continue;
                    }
                    
                    // Regular character
                    result_stream << c;
                    break;
                }
                
                case State::InLineComment: {
                    if (c == '\n' || c == '\r') {
                        state = State::Normal;
                        result_stream << c;
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
                    result_stream << ' ';
                    break;
                }
            }
        }
        
        // Get the normalized SQL
        std::string normalized_sql = result_stream.str();
        
        // Build parameters string (comma-separated)
        std::ostringstream params_stream;
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i > 0) {
                params_stream << ',';
            }
            params_stream << parameters[i];
        }
        
        return SqlNormalizeResult(normalized_sql, params_stream.str());
    }

    bool SqlNormalizer::isQuoteChar(char c) {
        return c == '\'' || c == '"' || c == '`';
    }

} // namespace pinpoint
