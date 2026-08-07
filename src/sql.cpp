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

        bool isSpace(char c) {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
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
        bool number_token_start_enable = true;

        // A removed comment is still a token separator: without this, removal
        // would butt the surrounding tokens together ("SELECT/*c*/1" ->
        // "SELECT1") and the digit after the comment would inherit the
        // identifier state of the character before it. Splice in one space
        // only when both neighbors are non-space, so removal never doubles
        // existing whitespace. Kept comments (remove_comments_ == false) are
        // left untouched to stay byte-compatible with the Java parser.
        auto separate_removed_comment = [&](size_t comment_end) {
            const char following = lookAhead1(sql, comment_end);
            if (!result.normalized_sql.empty() && !isSpace(result.normalized_sql.back()) &&
                following != '\0' && !isSpace(following)) {
                result.normalized_sql += ' ';
                number_token_start_enable = true;
            }
        };

        for (size_t i = 0; i < sql_length; ++i) {
            char c = sql[i];
            char next_c = lookAhead1(sql, i);

            switch (c) {
                case '/':
                    if (next_c == '*') {
                        i = readComment(sql, sql_length, i, "/*", "*/",
                            remove_comments_ ? nullptr : &result.normalized_sql);
                        if (remove_comments_) {
                            separate_removed_comment(i);
                        }
                    } else if (next_c == '/') {
                        i = readComment(sql, sql_length, i, "//", "\n",
                            remove_comments_ ? nullptr : &result.normalized_sql);
                        if (remove_comments_) {
                            separate_removed_comment(i);
                        }
                    } else {
                        number_token_start_enable = true;
                        result.normalized_sql += c;
                    }
                    break;

                case '-':
                    if (next_c == '-') {
                        i = readComment(sql, sql_length, i, "--", "\n",
                            remove_comments_ ? nullptr : &result.normalized_sql);
                        if (remove_comments_) {
                            separate_removed_comment(i);
                        }
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
                        // Both quotes of a doubled-quote escape are consumed
                        // but only one is emitted ('it''s' becomes 'it's').
                        // That corrupts the escape in the displayed SQL, but
                        // it is what the Java agent's DefaultSqlParser does
                        // and the output must stay byte-compatible with it
                        // (see JavaCombineBindValuesCases); do not "fix" this
                        // without the Java side changing first.
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
                    // The value is interpolated verbatim — quotes inside it
                    // are not escaped, matching the Java agent byte-for-byte
                    // (display-only string; it is never re-parsed here).
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

        // The parameter list already received its separator and content, so the
        // placeholder must be emitted even when the literal is unterminated
        // (e.g. the SQL was cut at max_sql_length mid-string); otherwise the
        // parameter indexes no longer line up with the normalized SQL.
        appendParameterIndex(result.normalized_sql, result.param_index);
        result.normalized_sql += kSymbolReplace;
        ++result.param_index;
        if (closed) {
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
