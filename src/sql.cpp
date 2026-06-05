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
#include <limits>

namespace pinpoint {

    namespace {
        constexpr char kNumberReplace = '#';
        constexpr char kSymbolReplace = '$';

        char lookAhead1(std::string_view sql, size_t index) {
            ++index;
            return index < sql.length() ? sql[index] : '\0';
        }

        bool isDigit(char c) {
            return std::isdigit(static_cast<unsigned char>(c)) != 0;
        }

        void appendParameterSeparator(SqlNormalizeResult& result) {
            if (!result.parameters.empty()) {
                result.parameters += ',';
            }
        }

        void appendParameterChar(SqlNormalizeResult& result, char ch) {
            if (ch == ',') {
                result.parameters += ",,";
            } else {
                result.parameters += ch;
            }
        }

        bool parseIndex(std::string_view text, size_t& index) {
            if (text.empty()) {
                return false;
            }

            size_t value = 0;
            for (char ch : text) {
                if (!isDigit(ch)) {
                    return false;
                }
                const size_t digit = static_cast<size_t>(ch - '0');
                if (value > (std::numeric_limits<size_t>::max() - digit) / 10) {
                    return false;
                }
                value = (value * 10) + digit;
            }

            index = value;
            return true;
        }
    } // namespace

    SqlNormalizer::SqlNormalizer(size_t max_sql_length, bool remove_comments)
        : max_sql_length_(max_sql_length), remove_comments_(remove_comments) {
    }

    SqlNormalizeResult SqlNormalizer::normalize(std::string_view sql) const {
        auto result = SqlNormalizeResult();
        if (sql.empty()) {
            return result;
        }
        
        // Limit SQL length to prevent memory issues.
        sql = sql.substr(0, std::min(sql.length(), max_sql_length_));
        const size_t sql_length = sql.length();
        result.normalized_sql.reserve(sql_length);
        result.parameters.reserve(64);

        // Tracks whether the next digit begins a numeric literal. Mirrors the Java
        // ParserContext.numberTokenStartEnable flag: a digit that follows an identifier
        // character (e.g. "col1") is part of the identifier, not a literal.
        bool number_token_start_enable = true;

        for (size_t i = 0; i < sql_length; ++i) {
            char c = sql[i];
            char next_c = lookAhead1(sql, i);

            switch (c) {
                case '/':
                    if (next_c == '*') {
                        i = readComment(sql, sql_length, i, "/*", "*/",
                            remove_comments_ ? nullptr : &result.normalized_sql);
                    } else if (next_c == '/') {
                        i = readComment(sql, sql_length, i, "//", "\n",
                            remove_comments_ ? nullptr : &result.normalized_sql);
                    } else {
                        number_token_start_enable = true;
                        result.normalized_sql += c;
                    }
                    break;

                case '-':
                    if (next_c == '-') {
                        i = readComment(sql, sql_length, i, "--", "\n",
                            remove_comments_ ? nullptr : &result.normalized_sql);
                    } else {
                        number_token_start_enable = true;
                        result.normalized_sql += c;
                    }
                    break;

                case '\'':
                    i = handleStringLiteral(sql, sql_length, i, c, result);
                    break;

                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9':
                    if (number_token_start_enable) {
                        i = handleNumericLiteral(sql, sql_length, i, result);
                    } else {
                        result.normalized_sql += c;
                    }
                    break;

                case ' ': case '\t': case '\n': case '\r':
                case '*': case '+': case '%': case '=': case '<': case '>':
                case '&': case '|': case '^': case '~': case '!':
                case '(': case ')': case ',': case ';':
                    number_token_start_enable = true;
                    result.normalized_sql += c;
                    break;

                case '$':
                    if (next_c >= '0' && next_c <= '9') {
                        number_token_start_enable = false;
                    }
                    result.normalized_sql += c;
                    break;

                case '.': case '_': case '@': case ':':
                    number_token_start_enable = false;
                    result.normalized_sql += c;
                    break;

                default:
                    number_token_start_enable = updateNumberTokenStartEnable(c, next_c, number_token_start_enable);
                    result.normalized_sql += c;
                    break;
            }
        }

        return result;
    }

    std::string SqlNormalizer::combineOutputParams(std::string_view sql, const std::vector<std::string>& output_params) const {
        if (sql.empty()) {
            return "";
        }

        const size_t length = sql.length();
        std::string normalized;
        normalized.reserve(length + 16);

        for (size_t i = 0; i < length; ++i) {
            const char ch = sql[i];
            switch (ch) {
                case '/':
                    if (lookAhead1(sql, i) == '*') {
                        i = readComment(sql, length, i, "/*", "*/", &normalized);
                    } else if (lookAhead1(sql, i) == '/') {
                        i = readComment(sql, length, i, "//", "\n", &normalized);
                    } else {
                        normalized += ch;
                    }
                    break;

                case '-':
                    if (lookAhead1(sql, i) == '-') {
                        i = readComment(sql, length, i, "--", "\n", &normalized);
                    } else {
                        normalized += ch;
                    }
                    break;

                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9': {
                    if (lookAhead1(sql, i) == '\0') {
                        normalized += ch;
                        break;
                    }

                    std::string output_index;
                    output_index += ch;
                    ++i;

                    bool token_done = false;
                    for (; i < length; ++i) {
                        const char state_ch = sql[i];
                        if (isDigit(state_ch)) {
                            if (lookAhead1(sql, i) == '\0') {
                                output_index += state_ch;
                                normalized += output_index;
                                token_done = true;
                                break;
                            }
                            output_index += state_ch;
                            continue;
                        }

                        if (state_ch == kNumberReplace || state_ch == kSymbolReplace) {
                            size_t index = 0;
                            if (parseIndex(output_index, index) && index < output_params.size()) {
                                normalized += output_params[index];
                            } else {
                                normalized += output_index;
                                normalized += state_ch;
                            }
                            token_done = true;
                            break;
                        }

                        normalized += output_index;
                        --i;
                        token_done = true;
                        break;
                    }

                    if (!token_done) {
                        normalized += output_index;
                    }
                    break;
                }

                default:
                    normalized += ch;
                    break;
            }
        }

        return normalized;
    }

    std::string SqlNormalizer::combineBindValues(std::string_view sql, const std::vector<std::string>& bind_values) const {
        if (sql.empty() || bind_values.empty()) {
            return std::string(sql);
        }

        const size_t length = sql.length();
        std::string result;
        result.reserve(length + 16);

        bool in_quotes = false;
        char quote_char = 0;
        size_t bind_index = 0;

        for (size_t i = 0; i < length; ++i) {
            const char ch = sql[i];
            if (in_quotes) {
                if ((ch == '\'' || ch == '"') && ch == quote_char) {
                    if (lookAhead1(sql, i) == quote_char) {
                        result += ch;
                        ++i;
                        continue;
                    }
                    in_quotes = false;
                    quote_char = 0;
                }
                result += ch;
                continue;
            }

            if (ch == '/') {
                if (lookAhead1(sql, i) == '*') {
                    i = readComment(sql, length, i, "/*", "*/", &result);
                } else if (lookAhead1(sql, i) == '/') {
                    i = readComment(sql, length, i, "//", "\n", &result);
                } else {
                    result += ch;
                }
            } else if (ch == '-') {
                if (lookAhead1(sql, i) == '-') {
                    i = readComment(sql, length, i, "--", "\n", &result);
                } else {
                    result += ch;
                }
            } else if (ch == '\'' || ch == '"') {
                in_quotes = true;
                quote_char = ch;
                result += ch;
            } else if (ch == '?') {
                if (bind_index < bind_values.size()) {
                    result += '\'';
                    result += bind_values[bind_index++];
                    result += '\'';
                }
            } else {
                result += ch;
            }
        }

        return result;
    }

    size_t SqlNormalizer::handleStringLiteral(std::string_view sql, size_t sql_length, size_t start_idx, char quote_char, SqlNormalizeResult& result) {
        if (start_idx + 1 < sql_length && sql[start_idx + 1] == quote_char) {
            result.normalized_sql += quote_char;
            result.normalized_sql += quote_char;
            return start_idx + 1;
        }

        result.normalized_sql += quote_char;
        appendParameterSeparator(result);

        size_t current_idx = start_idx + 1;
        for (; current_idx < sql_length; ++current_idx) {
            const char state_ch = sql[current_idx];
            if (state_ch == quote_char) {
                if (current_idx + 1 < sql_length && sql[current_idx + 1] == quote_char) {
                    ++current_idx;
                    result.parameters += "''";
                    continue;
                } else {
                    result.normalized_sql += std::to_string(result.param_index);
                    result.normalized_sql += kSymbolReplace;
                    result.normalized_sql += quote_char;
                    ++result.param_index;
                    break;
                }
            }
            appendParameterChar(result, state_ch);
        }

        return current_idx;
    }

    size_t SqlNormalizer::handleNumericLiteral(std::string_view sql, size_t sql_length, size_t start_idx, SqlNormalizeResult& result) {
        appendParameterSeparator(result);
        result.normalized_sql += std::to_string(result.param_index);
        result.normalized_sql += kNumberReplace;
        ++result.param_index;

        size_t current_idx = start_idx + 1;
        while (current_idx < sql_length) {
            const char state_ch = sql[current_idx];
            if (isDigit(state_ch) || state_ch == '.' || state_ch == 'E' || state_ch == 'e') {
                ++current_idx;
            } else {
                break;
            }
        }

        result.parameters.append(sql.substr(start_idx, current_idx - start_idx));
        return current_idx - 1;
    }

    bool SqlNormalizer::updateNumberTokenStartEnable(char c, char next_c, bool current) {
        switch (c) {
            // Whitespace
            case ' ': case '\t': case '\n': case '\r':
            // Operators
            case '*': case '+': case '%': case '=': case '<': case '>':
            case '&': case '|': case '^': case '~': case '!':
            // Separators and unary operators ('-' / '/' reach here only when they are
            // not the start of a comment or a negative numeric literal)
            case '(': case ')': case ',': case ';': case '-': case '/':
                return true;
            case '$':
                // A digit following a positional placeholder ($1, $2, ...) is not a literal.
                if (next_c >= '0' && next_c <= '9') {
                    return false;
                }
                return current;
            case '.': case '_': case '@': case ':':
                return false;
            default:
                // Letters begin an identifier (no number token); any other character may
                // precede a numeric literal.
                return (c < 'a' || c > 'z') && (c < 'A' || c > 'Z');
        }
    }

    size_t SqlNormalizer::readComment(std::string_view sql, size_t sql_length, size_t start_idx, std::string_view first_token,
        std::string_view end_token, std::string* writer) {
        const size_t search_start = std::min(start_idx + first_token.length(), sql_length);
        const size_t end_index = sql.find(end_token, search_start);

        size_t return_idx = sql_length;
        size_t append_end = sql_length;
        if (end_index != std::string_view::npos && end_index < sql_length) {
            return_idx = end_index + end_token.length() - 1;
            append_end = std::min(return_idx + 1, sql_length);
        }

        if (writer != nullptr && start_idx < append_end) {
            writer->append(sql.substr(start_idx, append_end - start_idx));
        }

        return return_idx;
    }

} // namespace pinpoint
