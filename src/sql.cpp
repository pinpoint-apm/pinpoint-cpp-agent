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
#include "utility.h"
#include <algorithm>
#include <cctype>
#include <charconv>

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

        // Appends the decimal digits of a parameter index without the
        // temporary std::string that std::to_string would create for every
        // replaced literal.
        void appendParameterIndex(std::string& out, int index) {
            char buf[16];
            const auto res = std::to_chars(buf, buf + sizeof(buf), index);
            out.append(buf, static_cast<size_t>(res.ptr - buf));
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
        
        // Limit SQL length to prevent memory issues. The cut is UTF-8 aware
        // so a truncated query never ends in a partial multibyte character.
        sql = sql.substr(0, utf8SafeCutLength(sql, max_sql_length_));
        if (sql.empty()) {
            return result;
        }
        const size_t sql_length = sql.length();
        result.normalized_sql.reserve(sql_length);
        result.parameters.reserve(64);

        // Tracks whether the next digit begins a numeric literal. Mirrors the Java
        // ParserContext.numberTokenStartEnable flag: a digit that follows an identifier
        // character (e.g. "col1") is part of the identifier, not a literal.
        // Comments never touch the flag, kept or removed: like Java, a removed
        // comment leaves no separator behind ("SELECT/*c*/1" -> "SELECT1").
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

                default:
                    // Whitespace, operators, '$', '.', '_', '@' and ':' all
                    // land here too; updateNumberTokenStartEnable carries
                    // their per-character rules.
                    number_token_start_enable = updateNumberTokenStartEnable(c, next_c, number_token_start_enable);
                    result.normalized_sql += c;
                    break;
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
        bool closed = false;
        for (; current_idx < sql_length; ++current_idx) {
            const char state_ch = sql[current_idx];
            if (state_ch == quote_char) {
                if (current_idx + 1 < sql_length && sql[current_idx + 1] == quote_char) {
                    ++current_idx;
                    result.parameters += "''";
                    continue;
                }
                closed = true;
                break;
            }
            appendParameterChar(result, state_ch);
        }

        // Java/Go parity: an unterminated literal (unbalanced quote, or SQL
        // cut at max_sql_length mid-string) keeps only the opening quote. Its
        // content is still recorded in the parameters, but no placeholder is
        // emitted.
        if (closed) {
            appendParameterIndex(result.normalized_sql, result.param_index);
            result.normalized_sql += kSymbolReplace;
            ++result.param_index;
            result.normalized_sql += quote_char;
        }

        return current_idx;
    }

    size_t SqlNormalizer::handleNumericLiteral(std::string_view sql, size_t sql_length, size_t start_idx, SqlNormalizeResult& result) {
        appendParameterSeparator(result);
        appendParameterIndex(result.normalized_sql, result.param_index);
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
                // precede a numeric literal. Bytes of a multibyte UTF-8 character land
                // here and return true, so a digit after a non-ASCII identifier is
                // extracted ("테이블1" -> "테이블0#"). The Java agent behaves the same:
                // ParserContext.isNumberTokenStart is this exact ASCII-only check on
                // UTF-16 chars. Do not "fix" this without the Java side changing first —
                // SQL metadata/UIDs must stay identical across agents.
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
