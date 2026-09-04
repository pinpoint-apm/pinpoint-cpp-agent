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

#include "../src/sql.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <vector>

namespace pinpoint {

class SqlTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Default: comments are kept, like the Java agent.
        normalizer_ = std::make_unique<SqlNormalizer>();
        // Opt-in comment removal (Sql.RemoveComments=true).
        remove_comments_ = std::make_unique<SqlNormalizer>(2048, true);
    }

    void TearDown() override {
        normalizer_.reset();
        remove_comments_.reset();
    }

protected:
    std::unique_ptr<SqlNormalizer> normalizer_;
    std::unique_ptr<SqlNormalizer> remove_comments_;
};

static std::vector<std::string> ParseOutputParameter(std::string_view output_params) {
    if (output_params.empty()) {
        return {};
    }

    std::vector<std::string> result;
    std::string params;
    for (size_t index = 0; index < output_params.length(); ++index) {
        const char ch = output_params[index];
        if (ch == ',') {
            const char ahead = (index + 1 < output_params.length()) ? output_params[index + 1] : '\0';
            if (ahead == ',') {
                params += ',';
                ++index;
            } else {
                if (ahead == ' ') {
                    ++index;
                }
                result.push_back(params);
                params.clear();
            }
        } else {
            params += ch;
        }
    }

    result.push_back(params);
    return result;
}

static void ExpectNormalize(const SqlNormalizer& normalizer, std::string_view sql,
    std::string_view expected_normalized, std::string_view expected_params = "") {
    auto result = normalizer.normalize(sql);
    EXPECT_EQ(result.normalized_sql, expected_normalized);
    EXPECT_EQ(result.parameters, expected_params);
}

// ---- Java OutputParameterParser/DefaultSqlParser combine ports -------------
//
// Test-only reimplementations of the Java agent's combine step (the Pinpoint
// web UI recombines normalized SQL with its extracted parameters). Production
// never combines — these live here purely to round-trip-verify normalize()
// against the Java parser, byte for byte.

static char LookAhead1(std::string_view sql, size_t index) {
    ++index;
    return index < sql.length() ? sql[index] : '\0';
}

static bool IsAsciiDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

static bool ParseIndex(std::string_view text, size_t& index) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), index);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

// Mirror of SqlNormalizer::readComment: copies the comment (start token
// through end token inclusive) into writer and returns the index of the
// comment's last character.
static size_t ReadComment(std::string_view sql, size_t sql_length, size_t start_idx,
    std::string_view first_token, std::string_view end_token, std::string* writer) {
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

static std::string combineOutputParams(std::string_view sql, const std::vector<std::string>& output_params) {
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
                if (LookAhead1(sql, i) == '*') {
                    i = ReadComment(sql, length, i, "/*", "*/", &normalized);
                } else if (LookAhead1(sql, i) == '/') {
                    i = ReadComment(sql, length, i, "//", "\n", &normalized);
                } else {
                    normalized += ch;
                }
                break;

            case '-':
                if (LookAhead1(sql, i) == '-') {
                    i = ReadComment(sql, length, i, "--", "\n", &normalized);
                } else {
                    normalized += ch;
                }
                break;

            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9': {
                if (LookAhead1(sql, i) == '\0') {
                    normalized += ch;
                    break;
                }

                std::string output_index;
                output_index += ch;
                ++i;

                bool token_done = false;
                for (; i < length; ++i) {
                    const char state_ch = sql[i];
                    if (IsAsciiDigit(state_ch)) {
                        if (LookAhead1(sql, i) == '\0') {
                            output_index += state_ch;
                            normalized += output_index;
                            token_done = true;
                            break;
                        }
                        output_index += state_ch;
                        continue;
                    }

                    if (state_ch == '#' || state_ch == '$') {
                        size_t index = 0;
                        if (ParseIndex(output_index, index) && index < output_params.size()) {
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

static void ExpectJavaDefaultNormalize(std::string_view sql, std::string_view expected_normalized,
    std::string_view expected_params = "") {
    SqlNormalizer java_default(2048, false);
    ExpectNormalize(java_default, sql, expected_normalized, expected_params);
}

static void ExpectJavaCombine(std::string_view original_sql, std::string_view normalized_sql,
    std::string_view output_params) {
    SqlNormalizer java_default(2048, false);
    ExpectNormalize(java_default, original_sql, normalized_sql, output_params);
    EXPECT_EQ(combineOutputParams(normalized_sql, ParseOutputParameter(output_params)), original_sql);
}

// ========== Basic Normalization Tests ==========

TEST_F(SqlTest, EmptySqlTest) {
    auto result = normalizer_->normalize("");
    EXPECT_EQ(result.normalized_sql, "");
    EXPECT_EQ(result.parameters, "");
}

// Test simple SQL normalization
TEST_F(SqlTest, SimpleSqlTest) {
    auto result = normalizer_->normalize("SELECT * FROM users");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users");
    EXPECT_EQ(result.parameters, "");
}

// ========== Parameter Replacement Tests ==========

// Test numeric parameter replacement with indexing
TEST_F(SqlTest, NumericParameterReplacementTest) {
    auto result = normalizer_->normalize("SELECT * FROM users WHERE id = 123");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE id = 0#");
    EXPECT_EQ(result.parameters, "123");
    
    result = normalizer_->normalize("SELECT * FROM users WHERE id = 123 AND age > 25");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE id = 0# AND age > 1#");
    EXPECT_EQ(result.parameters, "123,25");
}

// Test negative numbers
TEST_F(SqlTest, NegativeNumberParameterTest) {
    auto result = normalizer_->normalize("SELECT * FROM users WHERE balance = -100.50");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE balance = -0#");
    EXPECT_EQ(result.parameters, "100.50");
}

// Test decimal numbers
TEST_F(SqlTest, DecimalNumberParameterTest) {
    auto result = normalizer_->normalize("SELECT * FROM products WHERE price = 99.99");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM products WHERE price = 0#");
    EXPECT_EQ(result.parameters, "99.99");
}

// A digit following a non-ASCII (multibyte UTF-8) identifier character is
// extracted as a numeric literal: every byte of a multibyte character takes
// the default branch of updateNumberTokenStartEnable and re-enables number
// token start. The Java agent behaves identically — its
// ParserContext.isNumberTokenStart is the same ASCII-only check — so both
// agents normalize such SQL to the same bytes and the same UID. ASCII
// identifiers keep their digits. Do not "fix" this without the Java side
// changing first.
TEST_F(SqlTest, NonAsciiIdentifierDigitMatchesJavaAgentTest) {
    // ASCII identifier: the trailing digit is part of the identifier.
    auto result = normalizer_->normalize("SELECT col1 FROM users");
    EXPECT_EQ(result.normalized_sql, "SELECT col1 FROM users");
    EXPECT_EQ(result.parameters, "");

    // Non-ASCII identifier: the trailing digit is extracted, as in Java.
    result = normalizer_->normalize("SELECT * FROM 테이블1");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM 테이블0#");
    EXPECT_EQ(result.parameters, "1");
}

// Test multiple numbers with proper indexing
TEST_F(SqlTest, MultipleNumbersTest) {
    auto result = normalizer_->normalize("SELECT * FROM users WHERE age > 18 AND balance < 1000.0 AND score = 95");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE age > 0# AND balance < 1# AND score = 2#");
    EXPECT_EQ(result.parameters, "18,1000.0,95");
}

// Test string literals are replaced with indexed placeholders
TEST_F(SqlTest, StringLiteralReplacementTest) {
    auto result = normalizer_->normalize("SELECT * FROM users WHERE name = 'John Doe'");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE name = '0$'");
    EXPECT_EQ(result.parameters, "John Doe");
    
    result = normalizer_->normalize("INSERT INTO users (name, email) VALUES ('John', 'john@example.com')");
    EXPECT_EQ(result.normalized_sql, "INSERT INTO users (name, email) VALUES ('0$', '1$')");
    EXPECT_EQ(result.parameters, "John,john@example.com");
}



// Test mixed numeric and string literals
TEST_F(SqlTest, MixedParametersTest) {
    auto result = normalizer_->normalize("SELECT * FROM users WHERE id = 123 AND name = 'John' AND age > 25 AND status = 'active'");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE id = 0# AND name = '1$' AND age > 2# AND status = '3$'");
    EXPECT_EQ(result.parameters, "123,John,25,active");
}

// ========== Comment Removal Tests ==========

TEST_F(SqlTest, LineCommentRemovalTest) {
    auto result = remove_comments_->normalize("SELECT * FROM users -- This is a comment");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users ");
    EXPECT_EQ(result.parameters, "");
    
    result = remove_comments_->normalize("SELECT * FROM users -- Comment\nWHERE id = 1");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE id = 0#");
    EXPECT_EQ(result.parameters, "1");
}

TEST_F(SqlTest, BlockCommentRemovalTest) {
    auto result = remove_comments_->normalize("SELECT * /* This is a block comment */ FROM users");
    EXPECT_EQ(result.normalized_sql, "SELECT *  FROM users");
    EXPECT_EQ(result.parameters, "");
    
    result = remove_comments_->normalize("SELECT * /* Multi\nline\ncomment */ FROM users");
    EXPECT_EQ(result.normalized_sql, "SELECT *  FROM users");
    EXPECT_EQ(result.parameters, "");
}

// Test // single-line comment removal (matches Java agent behavior)
TEST_F(SqlTest, SlashSlashLineCommentRemovalTest) {
    auto result = remove_comments_->normalize("SELECT * FROM t // trailing comment\nWHERE a=1");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM t WHERE a=0#");
    EXPECT_EQ(result.parameters, "1");
}

// Test that a lone '/' (division operator) is preserved as a normal character
TEST_F(SqlTest, DivisionOperatorPreservedTest) {
    auto result = normalizer_->normalize("SELECT a/b FROM t");
    EXPECT_EQ(result.normalized_sql, "SELECT a/b FROM t");
    EXPECT_EQ(result.parameters, "");
}

TEST_F(SqlTest, MixedCommentsTest) {
    auto result = remove_comments_->normalize("SELECT * /* block */ FROM users -- line comment");
    EXPECT_EQ(result.normalized_sql, "SELECT *  FROM users ");
    EXPECT_EQ(result.parameters, "");
}

TEST_F(SqlTest, CommentsWithParametersTest) {
    auto result = remove_comments_->normalize("SELECT * FROM users /* ignore 123 */ WHERE id = 456 -- ignore :param");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users  WHERE id = 0# ");
    EXPECT_EQ(result.parameters, "456");
}

// Java parity (ParserContext with removeComments=true): a removed comment
// leaves nothing behind — no space is spliced in and the number-token flag is
// not touched, so the surrounding tokens merge exactly as Java merges them.
TEST_F(SqlTest, CommentRemovalLeavesNoSeparatorTest) {
    auto result = remove_comments_->normalize("SELECT/*c*/1 FROM t");
    EXPECT_EQ(result.normalized_sql, "SELECT1 FROM t");
    EXPECT_EQ(result.parameters, "");

    result = remove_comments_->normalize("SELECT a FROM t WHERE a=1--c\nAND b=2");
    EXPECT_EQ(result.normalized_sql, "SELECT a FROM t WHERE a=0#AND b=1#");
    EXPECT_EQ(result.parameters, "1,2");

    result = remove_comments_->normalize("SELECT col1/*c*/2 FROM t");
    EXPECT_EQ(result.normalized_sql, "SELECT col12 FROM t");
    EXPECT_EQ(result.parameters, "");

    result = remove_comments_->normalize("SELECT * /*c*/ FROM t");
    EXPECT_EQ(result.normalized_sql, "SELECT *  FROM t");
    EXPECT_EQ(result.parameters, "");

    result = remove_comments_->normalize("SELECT//c\n1 FROM t");
    EXPECT_EQ(result.normalized_sql, "SELECT1 FROM t");
    EXPECT_EQ(result.parameters, "");

    result = remove_comments_->normalize("SELECT//c\n FROM t");
    EXPECT_EQ(result.normalized_sql, "SELECT FROM t");
    EXPECT_EQ(result.parameters, "");
}

// ========== Whitespace Preservation Tests ==========

// Test basic SQL with preserved whitespace
TEST_F(SqlTest, WhitespacePreservationTest) {
    auto result = normalizer_->normalize("SELECT   *    FROM\n\n  users   WHERE\tid   =   123");
    EXPECT_EQ(result.normalized_sql, "SELECT   *    FROM\n\n  users   WHERE\tid   =   0#");
    EXPECT_EQ(result.parameters, "123");
    
    result = normalizer_->normalize("   SELECT * FROM users   ");
    EXPECT_EQ(result.normalized_sql, "   SELECT * FROM users   ");
    EXPECT_EQ(result.parameters, "");
}

// ========== String Literal Handling Tests ==========

// Test string literals with different quote types
TEST_F(SqlTest, StringLiteralHandlingTest) {
    auto result = normalizer_->normalize("SELECT * FROM users WHERE name = 'John''s Company'");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE name = '0$'");
    EXPECT_EQ(result.parameters, "John''s Company");
    
    result = normalizer_->normalize("SELECT * FROM users WHERE name = `user_name`");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE name = `user_name`");
    EXPECT_EQ(result.parameters, "");
}

// Test string literals with escaped quotes (backslash escape not handled, so string ends at backslash)
TEST_F(SqlTest, EscapedQuotesTest) {
    auto result = normalizer_->normalize("SELECT * FROM users WHERE name = 'John\\'s Company'");
    // The trailing quote opens an unterminated (empty) literal: like Java, only
    // the quote is kept and no placeholder is emitted.
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE name = '0$'s Company'");
    EXPECT_EQ(result.parameters, "John\\,");
}

// Test string literals with numbers inside (string literal replaced, numbers in string protected)
TEST_F(SqlTest, StringLiteralWithNumbersTest) {
    auto result = normalizer_->normalize("SELECT * FROM users WHERE phone = '123-456-7890' AND age > 25");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE phone = '0$' AND age > 1#");
    EXPECT_EQ(result.parameters, "123-456-7890,25");
}

// ========== Complex SQL Tests ==========

// Test complex SELECT query with one-pass processing
TEST_F(SqlTest, ComplexSelectQueryTest) {
    std::string complex_sql = R"(
        SELECT u.id, u.name, COUNT(o.id) as order_count
        FROM users u -- Main users table
        LEFT JOIN orders o ON u.id = o.user_id
        WHERE u.created_at > '2023-01-01'
          AND u.status = 'active'
          AND u.age > 18
        GROUP BY u.id, u.name
        HAVING COUNT(o.id) > 5
        ORDER BY order_count DESC
        LIMIT 100
    )";
    
    auto result = normalizer_->normalize(complex_sql);
    // Should replace both string literals and numeric literals with proper indexing
    EXPECT_TRUE(result.normalized_sql.find("u.created_at > '0$'") != std::string::npos);
    EXPECT_TRUE(result.normalized_sql.find("u.status = '1$'") != std::string::npos);
    EXPECT_TRUE(result.normalized_sql.find("u.age > 2#") != std::string::npos);
    EXPECT_TRUE(result.normalized_sql.find("COUNT(o.id) > 3#") != std::string::npos);
    EXPECT_TRUE(result.normalized_sql.find("LIMIT 4#") != std::string::npos);
    EXPECT_EQ(result.parameters, "2023-01-01,active,18,5,100");
}

// Test complex INSERT with mixed parameters
TEST_F(SqlTest, ComplexInsertQueryTest) {
    std::string complex_sql = R"(
        INSERT INTO user_activity_log (user_id, action, timestamp, score, metadata)
        VALUES (123, 'login', NOW(), 95.5, '{"ip":"192.168.1.1"}')
    )";
    
    auto result = normalizer_->normalize(complex_sql);
    // Should replace both string literals and numeric literals with proper indexing
    EXPECT_TRUE(result.normalized_sql.find("VALUES (0#, '1$', NOW(), 2#, '3$')") != std::string::npos);
    EXPECT_EQ(result.parameters, "123,login,95.5,{\"ip\":\"192.168.1.1\"}");
}

// ========== Edge Cases Tests ==========

// Test very long SQL (should be truncated)
TEST_F(SqlTest, VeryLongSqlTest) {
    std::string long_sql(5000, 'A'); // 5KB of 'A' characters
    long_sql = "SELECT * FROM users WHERE name = '" + long_sql + "'";
    
    auto result = normalizer_->normalize(long_sql);
    EXPECT_TRUE(result.normalized_sql.length() <= 2048); // Should be truncated to max_sql_length
}

// The agent's normalizer only enforces a hard memory cap, so a statement over
// the 64 KiB metadata cap is still normalized whole: that output is the SQL
// id/UID cache key, and Java likewise hashes the full normalized SQL and
// abbreviates only PSqlMetaData.sql (SqlCacheService).
TEST_F(SqlTest, NormalizesPastMetadataCapUpToHardCap) {
    SqlNormalizer agent_normalizer(kMaxNormalizedSqlLength);
    const std::string filler(70000, 'a');
    const std::string long_sql =
        "SELECT * FROM users WHERE name = 'x' AND note = '" + filler + "'";

    auto result = agent_normalizer.normalize(long_sql);

    EXPECT_EQ(result.normalized_sql,
              "SELECT * FROM users WHERE name = '0$' AND note = '1$'");
    EXPECT_EQ(result.parameters, "x," + filler);
}

TEST_F(SqlTest, StopsNormalizingAtHardCap) {
    SqlNormalizer agent_normalizer(kMaxNormalizedSqlLength);
    const std::string over_cap(kMaxNormalizedSqlLength + 1000, 'a');

    auto result = agent_normalizer.normalize("SELECT " + over_cap);

    EXPECT_EQ(result.normalized_sql.length(), kMaxNormalizedSqlLength);
}

// Test SQL with only comments
TEST_F(SqlTest, OnlyCommentsTest) {
    auto result = remove_comments_->normalize("/* This is only a comment */");
    EXPECT_EQ(result.normalized_sql, "");
    EXPECT_EQ(result.parameters, "");
}

// Test SQL with malformed quotes
TEST_F(SqlTest, MalformedQuotesTest) {
    auto result = normalizer_->normalize("SELECT * FROM users WHERE name = 'unclosed quote AND id = 123");
    // Should still work even with malformed quotes
    EXPECT_FALSE(result.normalized_sql.empty());
    EXPECT_TRUE(result.normalized_sql.find("SELECT * FROM users") != std::string::npos);
}

// The max_sql_length cut must never split a multibyte UTF-8 character: the
// normalized SQL and parameters flow into protobuf string fields that require
// valid UTF-8, and a partial character would make the collector reject or
// mangle the SQL metadata.
TEST_F(SqlTest, Utf8SafeTruncationTest) {
    // "SELECT '" is 8 bytes; each "가" (U+AC00) is 3 bytes (EA B0 80), so
    // the first spans bytes 8-10 and the second bytes 11-13. A limit of 12
    // lands after the first byte of the second "가" — the whole partial
    // character must be dropped, keeping 11 bytes.
    SqlNormalizer cut_mid_char(12);
    auto result = cut_mid_char.normalize("SELECT '\xEA\xB0\x80\xEA\xB0\x80'");
    EXPECT_EQ(result.parameters, "\xEA\xB0\x80");
    EXPECT_EQ(result.normalized_sql, "SELECT '");

    // A limit landing exactly on a character boundary keeps every byte.
    SqlNormalizer cut_on_boundary(14);
    result = cut_on_boundary.normalize("SELECT '\xEA\xB0\x80\xEA\xB0\x80'");
    EXPECT_EQ(result.parameters, "\xEA\xB0\x80\xEA\xB0\x80");

    // Pure ASCII truncation is unaffected.
    SqlNormalizer ascii_cut(8);
    result = ascii_cut.normalize("SELECT 123456");
    EXPECT_EQ(result.normalized_sql, "SELECT 0#");
    EXPECT_EQ(result.parameters, "1");
}

// An unterminated string literal (e.g. SQL truncated at max_sql_length in the
// middle of a string) keeps only its opening quote and emits no placeholder;
// its content is still recorded in the parameters (Java/Go parity).
TEST_F(SqlTest, UnterminatedStringLiteralNoPlaceholderTest) {
    auto result = normalizer_->normalize("SELECT * FROM t WHERE a = 'x' AND b = 'unclosed");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM t WHERE a = '0$' AND b = '");
    EXPECT_EQ(result.parameters, "x,unclosed");

    SqlNormalizer short_normalizer(20);
    result = short_normalizer.normalize("SELECT 'long literal cut off by the length limit'");
    EXPECT_EQ(result.normalized_sql, "SELECT '");
    EXPECT_EQ(result.parameters, "long literal");
}

// ========== One-Pass Integration Tests ==========

// Test complete one-pass normalization flow
TEST_F(SqlTest, OnePassNormalizationFlowTest) {
    std::string original_sql = R"(
        /* Get user data with filters */
        SELECT   u.id,  u.name  -- User info
        FROM     users    u
        WHERE    u.age  >  25    /* Adults only */
          AND    u.balance  =  1000.50
          AND    u.status  =  'active'  -- Active users
          AND    u.created_at > '2023-01-01'
        ORDER BY u.created_at DESC
        LIMIT    10
    )";
    
    auto result = normalizer_->normalize(original_sql);
    
    // Verify all transformations in one pass - whitespace is now preserved, comments removed
    EXPECT_TRUE(result.normalized_sql.find("u.age  >  0#") != std::string::npos);
    EXPECT_TRUE(result.normalized_sql.find("u.balance  =  1#") != std::string::npos);
    EXPECT_TRUE(result.normalized_sql.find("u.status  =  '2$'") != std::string::npos);
    EXPECT_TRUE(result.normalized_sql.find("u.created_at > '3$'") != std::string::npos);
    EXPECT_TRUE(result.normalized_sql.find("LIMIT    4#") != std::string::npos);
    EXPECT_EQ(result.parameters, "25,1000.50,active,2023-01-01,10");
}

TEST_F(SqlTest, ParameterIndexingAccuracyTest) {
    auto result = normalizer_->normalize("SELECT 1, 'literal', 2, 'another', 3.14, 42");
    EXPECT_EQ(result.normalized_sql, "SELECT 0#, '1$', 2#, '3$', 4#, 5#");
    EXPECT_EQ(result.parameters, "1,literal,2,another,3.14,42");
}

TEST_F(SqlTest, NestedQuotesAndCommentsTest) {
    auto result = remove_comments_->normalize("SELECT 'Comment /* not really */' FROM table WHERE id = 123 -- 'not a string'");
    EXPECT_EQ(result.normalized_sql, "SELECT '0$' FROM table WHERE id = 1# ");
    EXPECT_EQ(result.parameters, "Comment /* not really */,123");
}

// ========== Additional Edge Case Tests ==========

// Test custom max_sql_length constructor
TEST_F(SqlTest, CustomMaxSqlLengthTest) {
    SqlNormalizer short_normalizer(30);
    // SQL is longer than 30 chars, should be truncated during processing
    auto result = short_normalizer.normalize("SELECT * FROM users WHERE id = 123 AND name = 'John'");
    EXPECT_TRUE(result.normalized_sql.length() <= 30);
}

TEST_F(SqlTest, EmptyStringLiteralTest) {
    auto result = normalizer_->normalize("SELECT * FROM users WHERE name = ''");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE name = ''");
    EXPECT_EQ(result.parameters, "");

    result = normalizer_->normalize("INSERT INTO t VALUES ('', 'hello')");
    EXPECT_EQ(result.normalized_sql, "INSERT INTO t VALUES ('', '0$')");
    EXPECT_EQ(result.parameters, "hello");
}

// Test consecutive empty string literals (matches Java DefaultSqlNormalizer behavior)
TEST_F(SqlTest, ConsecutiveEscapedQuotesTest) {
    auto result = normalizer_->normalize("SELECT * FROM t WHERE v = ''''");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM t WHERE v = ''''");
    EXPECT_EQ(result.parameters, "");
}

TEST_F(SqlTest, UnclosedBlockCommentTest) {
    auto result = remove_comments_->normalize("SELECT * FROM users /* unclosed comment");
    // Everything after /* should be consumed as comment
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users ");
    EXPECT_EQ(result.parameters, "");
}

TEST_F(SqlTest, WhitespaceOnlySqlTest) {
    auto result = normalizer_->normalize("   \t\n  ");
    EXPECT_EQ(result.normalized_sql, "   \t\n  ");
    EXPECT_EQ(result.parameters, "");
}

// Test subtraction operator (minus not followed by digit should not be treated as negative number)
TEST_F(SqlTest, SubtractionOperatorTest) {
    auto result = normalizer_->normalize("SELECT a - b FROM t");
    EXPECT_EQ(result.normalized_sql, "SELECT a - b FROM t");
    EXPECT_EQ(result.parameters, "");
}

// Test minus between two numbers
TEST_F(SqlTest, MinusBetweenNumbersTest) {
    auto result = normalizer_->normalize("SELECT 10 - 3 FROM t");
    EXPECT_EQ(result.normalized_sql, "SELECT 0# - 1# FROM t");
    EXPECT_EQ(result.parameters, "10,3");

    auto result2 = normalizer_->normalize("SELECT 10 -3 FROM t");
    EXPECT_EQ(result2.normalized_sql, "SELECT 0# -1# FROM t");
    EXPECT_EQ(result2.parameters, "10,3");
}

// Test string parameter containing comma (ambiguous with parameter separator).
// Commas inside a string value are escaped as ",," so they are not confused with the
// ',' separator between parameters (matches Java ParameterBuilder.appendSeparatorCheck).
TEST_F(SqlTest, StringWithCommaParameterTest) {
    auto result = normalizer_->normalize("SELECT * FROM t WHERE name = 'Doe, John'");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM t WHERE name = '0$'");
    EXPECT_EQ(result.parameters, "Doe,, John");
}

// Test multiple string parameters where one contains a comma. The embedded comma is
// escaped (",,") while the separator between the two parameters stays a single ','.
TEST_F(SqlTest, StringWithCommaAmongMultipleParametersTest) {
    auto result = normalizer_->normalize(
        "INSERT INTO t (a, b) VALUES ('Doe, John', 'plain')");
    EXPECT_EQ(result.normalized_sql, "INSERT INTO t (a, b) VALUES ('0$', '1$')");
    EXPECT_EQ(result.parameters, "Doe,, John,plain");
}

TEST_F(SqlTest, ConsecutiveBlockCommentsTest) {
    auto result = remove_comments_->normalize("SELECT /* c1 */ * /* c2 */ FROM users");
    EXPECT_EQ(result.normalized_sql, "SELECT  *  FROM users");
    EXPECT_EQ(result.parameters, "");
}

// Test block comment immediately followed by string literal
TEST_F(SqlTest, BlockCommentFollowedByStringTest) {
    auto result = remove_comments_->normalize("SELECT /* comment */'hello' FROM t");
    EXPECT_EQ(result.normalized_sql, "SELECT '0$' FROM t");
    EXPECT_EQ(result.parameters, "hello");
}

// Test number at very end of SQL
TEST_F(SqlTest, NumberAtEndOfSqlTest) {
    auto result = normalizer_->normalize("SELECT 42");
    EXPECT_EQ(result.normalized_sql, "SELECT 0#");
    EXPECT_EQ(result.parameters, "42");
}

// Test line comment with \r\n line endings
TEST_F(SqlTest, LineCommentCRLFTest) {
    auto result = remove_comments_->normalize("SELECT * -- comment\r\nFROM users");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users");
    EXPECT_EQ(result.parameters, "");
}

// Test line comment with only \r (carriage return)
TEST_F(SqlTest, LineCommentCROnlyTest) {
    auto result = remove_comments_->normalize("SELECT * -- comment\rFROM users");
    EXPECT_EQ(result.normalized_sql, "SELECT * ");
    EXPECT_EQ(result.parameters, "");
}

// Test numbers embedded in identifiers (e.g., table1, col2). A digit that follows an
// identifier character is part of the identifier and is NOT treated as a numeric literal
// (matches Java ParserContext.numberTokenStartEnable).
TEST_F(SqlTest, NumbersInIdentifiersTest) {
    auto result = normalizer_->normalize("SELECT col1 FROM table2 WHERE id = 5");
    // col1 and table2 are left intact; only the standalone 5 is a literal.
    EXPECT_EQ(result.normalized_sql, "SELECT col1 FROM table2 WHERE id = 0#");
    EXPECT_EQ(result.parameters, "5");
}

// Test identifiers ending in a digit with no standalone numeric literal present.
TEST_F(SqlTest, IdentifierEndingInDigitTest) {
    auto result = normalizer_->normalize("SELECT col1 FROM t1");
    EXPECT_EQ(result.normalized_sql, "SELECT col1 FROM t1");
    EXPECT_EQ(result.parameters, "");

    // Multi-digit suffix and a digit-only-after-letter identifier.
    result = normalizer_->normalize("SELECT a1, b22, c3 FROM t9");
    EXPECT_EQ(result.normalized_sql, "SELECT a1, b22, c3 FROM t9");
    EXPECT_EQ(result.parameters, "");
}

// Test truncation cutting through a string literal
TEST_F(SqlTest, TruncationMidStringLiteralTest) {
    SqlNormalizer tiny_normalizer(20);
    // "SELECT name = 'very long string value'" is > 20 chars
    // Truncation at 20 chars will cut inside the string literal
    auto result = tiny_normalizer.normalize("SELECT name = 'very long string value'");
    // Should not crash; malformed string handling should kick in
    EXPECT_FALSE(result.normalized_sql.empty());
}

// Test truncation cutting through a number
TEST_F(SqlTest, TruncationMidNumberTest) {
    SqlNormalizer tiny_normalizer(25);
    auto result = tiny_normalizer.normalize("SELECT * FROM t WHERE x = 123456789");
    // Should not crash; number may be partially captured
    EXPECT_FALSE(result.normalized_sql.empty());
}

// Test truncation cutting through a block comment
TEST_F(SqlTest, TruncationMidBlockCommentTest) {
    SqlNormalizer tiny_normalizer(20);
    auto result = tiny_normalizer.normalize("SELECT * /* very long block comment */ FROM t");
    // Should not crash
    EXPECT_FALSE(result.normalized_sql.empty());
}

// Test SQL with many parameters (verify indexing stays correct)
TEST_F(SqlTest, ManyParametersIndexingTest) {
    auto result = normalizer_->normalize(
        "INSERT INTO t VALUES (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 'a', 'b')");
    EXPECT_EQ(result.normalized_sql,
        "INSERT INTO t VALUES (0#, 1#, 2#, 3#, 4#, 5#, 6#, 7#, 8#, 9#, '10$', '11$')");
    // Verify double-digit indices work
    EXPECT_TRUE(result.normalized_sql.find("'10$'") != std::string::npos);
    EXPECT_TRUE(result.normalized_sql.find("'11$'") != std::string::npos);
    EXPECT_EQ(result.parameters, "1,2,3,4,5,6,7,8,9,10,a,b");
}

TEST_F(SqlTest, UpdateStatementTest) {
    auto result = normalizer_->normalize(
        "UPDATE users SET name = 'Alice', age = 30 WHERE id = 1");
    EXPECT_EQ(result.normalized_sql,
        "UPDATE users SET name = '0$', age = 1# WHERE id = 2#");
    EXPECT_EQ(result.parameters, "Alice,30,1");
}

TEST_F(SqlTest, DeleteStatementTest) {
    auto result = normalizer_->normalize(
        "DELETE FROM users WHERE id = 42 AND status = 'inactive'");
    EXPECT_EQ(result.normalized_sql,
        "DELETE FROM users WHERE id = 0# AND status = '1$'");
    EXPECT_EQ(result.parameters, "42,inactive");
}

// Test IN clause with multiple values
TEST_F(SqlTest, InClauseTest) {
    auto result = normalizer_->normalize(
        "SELECT * FROM users WHERE id IN (1, 2, 3) AND name IN ('a', 'b')");
    EXPECT_EQ(result.normalized_sql,
        "SELECT * FROM users WHERE id IN (0#, 1#, 2#) AND name IN ('3$', '4$')");
    EXPECT_EQ(result.parameters, "1,2,3,a,b");
}

TEST_F(SqlTest, BetweenClauseTest) {
    auto result = normalizer_->normalize(
        "SELECT * FROM orders WHERE amount BETWEEN 100.0 AND 500.0");
    EXPECT_EQ(result.normalized_sql,
        "SELECT * FROM orders WHERE amount BETWEEN 0# AND 1#");
    EXPECT_EQ(result.parameters, "100.0,500.0");
}

// Test string literal containing comment-like patterns
TEST_F(SqlTest, StringWithCommentPatternsTest) {
    auto result = normalizer_->normalize("SELECT * FROM t WHERE v = 'has -- dash'");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM t WHERE v = '0$'");
    EXPECT_EQ(result.parameters, "has -- dash");

    result = normalizer_->normalize("SELECT * FROM t WHERE v = 'has // slash'");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM t WHERE v = '0$'");
    EXPECT_EQ(result.parameters, "has // slash");
}

// Test number with multiple decimal points (e.g., IP-like pattern)
TEST_F(SqlTest, MultipleDecimalPointsTest) {
    // "1.2.3" - the normalizer reads digits and dots, so it will consume "1.2.3" as one token
    auto result = normalizer_->normalize("SELECT * FROM t WHERE v = 1.2.3");
    // Document current behavior: treated as a single numeric token
    EXPECT_TRUE(result.parameters.find("1.2.3") != std::string::npos);
}

// ========== Java parity golden cases ==========
//
// Expected values are the Java agent's DefaultSqlNormalizer output
// (removeComments=false, the default on both sides). The normalized SQL is
// the SQL id/UID cache key and PSqlMetaData.sql, so it must match byte for
// byte.

TEST_F(SqlTest, JavaParityGoldenCases) {
    ExpectNormalize(*normalizer_,
        "select * from t where a = 1.5e3 and b = 'it''s' and c = \"col1\" -- comment",
        "select * from t where a = 0# and b = '1$' and c = \"col1\" -- comment", "1.5e3,it''s");
    ExpectNormalize(*normalizer_,
        "a = -1 and b = 1e-3 and c = 0x1F",
        "a = -0# and b = 1#-2# and c = 3#x1F", "1,1e,3,0");
    ExpectNormalize(*normalizer_,
        "select t1.col2, t1.5 from t where id = 10 and name = 'a,b' and n2 = 'x''y'",
        "select t1.col2, t1.5 from t where id = 0# and name = '1$' and n2 = '2$'", "10,a,,b,x''y");
    ExpectNormalize(*normalizer_,
        "s = 'a\\'b' and n = 3",
        "s = '0$'b'", "a\\, and n = 3");
    ExpectNormalize(*normalizer_,
        "select /*+ INDEX(t idx) */ 1, \"2\", '' , 'z' from t /* multi\n line 42 */ where x = ?",
        "select /*+ INDEX(t idx) */ 0#, \"1#\", '' , '2$' from t /* multi\n line 42 */ where x = ?", "1,2,z");
    ExpectNormalize(*normalizer_,
        "SELECT/*c*/1 FROM t WHERE 테이블1 = 2 and 名前 = '値'",
        "SELECT/*c*/1 FROM t WHERE 테이블0# = 1# and 名前 = '2$'", "1,2,値");
    ExpectNormalize(*normalizer_,
        "select $1, $2 from t where a=$3 and b = 4",
        "select $1, $2 from t where a=$3 and b = 0#", "4");
    ExpectNormalize(*normalizer_, "select 'abc", "select '", "abc");
    ExpectNormalize(*normalizer_,
        "insert into t values (1,2,3), ('a','b','c') // trailing",
        "insert into t values (0#,1#,2#), ('3$','4$','5$') // trailing", "1,2,3,a,b,c");
}

// ========== Ported Java DefaultSqlNormalizer Tests ==========

TEST_F(SqlTest, JavaDefaultComplexCases) {
    ExpectJavaCombine("select * from table a = 1 and b=50 and c=? and d='11'",
        "select * from table a = 0# and b=1# and c=? and d='2$'", "1,50,11");
    ExpectJavaCombine("select * from table a = -1 and b=-50 and c=? and d='-11'",
        "select * from table a = -0# and b=-1# and c=? and d='2$'", "1,50,-11");
    ExpectJavaCombine("select * from table a = +1 and b=+50 and c=? and d='+11'",
        "select * from table a = +0# and b=+1# and c=? and d='2$'", "1,50,+11");
    ExpectJavaCombine("select * from table a = 1/*test*/ and b=50/*test*/ and c=? and d='11'",
        "select * from table a = 0#/*test*/ and b=1#/*test*/ and c=? and d='2$'", "1,50,11");
    ExpectJavaCombine("select ZIPCODE,CITY from ZIPCODE", "select ZIPCODE,CITY from ZIPCODE", "");
    ExpectJavaCombine("select a.ZIPCODE,a.CITY from ZIPCODE as a", "select a.ZIPCODE,a.CITY from ZIPCODE as a", "");
    ExpectJavaCombine("select ZIPCODE,123 from ZIPCODE", "select ZIPCODE,0# from ZIPCODE", "123");
    ExpectJavaCombine("SELECT * from table a=123 and b='abc' and c=1-3",
        "SELECT * from table a=0# and b='1$' and c=2#-3#", "123,abc,1,3");
    ExpectJavaCombine("SYSTEM_RANGE(1, 10)", "SYSTEM_RANGE(0#, 1#)", "1,10");
}

TEST_F(SqlTest, JavaDefaultObjectAndIdentifierCases) {
    ExpectJavaDefaultNormalize("test.abc", "test.abc", "");
    ExpectJavaDefaultNormalize("test.abc123", "test.abc123", "");
    ExpectJavaDefaultNormalize("test.123", "test.123", "");
}

TEST_F(SqlTest, JavaDefaultNumberStateCases) {
    ExpectJavaCombine("123", "0#", "123");
    ExpectJavaCombine("-123", "-0#", "123");
    ExpectJavaCombine("+123", "+0#", "123");
    ExpectJavaCombine("1.23", "0#", "1.23");
    ExpectJavaCombine("1.23.34", "0#", "1.23.34");
    ExpectJavaCombine("123 456", "0# 1#", "123,456");
    ExpectJavaCombine("1.23 4.56", "0# 1#", "1.23,4.56");
    ExpectJavaCombine("1.23-4.56", "0#-1#", "1.23,4.56");

    ExpectJavaCombine("1<2", "0#<1#", "1,2");
    ExpectJavaCombine("1< 2", "0#< 1#", "1,2");
    ExpectJavaCombine("(1< 2)", "(0#< 1#)", "1,2");

    ExpectJavaDefaultNormalize("-- 1.23", "-- 1.23", "");
    ExpectJavaCombine("- -1.23", "- -0#", "1.23");
    ExpectJavaDefaultNormalize("--1.23", "--1.23", "");
    ExpectJavaDefaultNormalize("/* 1.23 */", "/* 1.23 */", "");
    ExpectJavaDefaultNormalize("/*1.23*/", "/*1.23*/", "");
    ExpectJavaDefaultNormalize("/* 1.23 \n*/", "/* 1.23 \n*/", "");

    ExpectJavaDefaultNormalize("test123", "test123", "");
    ExpectJavaDefaultNormalize("test_123", "test_123", "");
    ExpectJavaCombine("test_ 123", "test_ 0#", "123");
    ExpectJavaCombine("123tst", "0#tst", "123");
}

TEST_F(SqlTest, JavaDefaultExponentNumberCases) {
    ExpectJavaCombine("1.23e", "0#", "1.23e");
    ExpectJavaCombine("1.23E", "0#", "1.23E");
    ExpectJavaCombine("1.4e-10", "0#-1#", "1.4e,10");
    ExpectJavaCombine("123 ", "0# ", "123");
}

TEST_F(SqlTest, JavaDefaultSingleLineCommentCases) {
    ExpectJavaDefaultNormalize("--", "--", "");
    ExpectJavaDefaultNormalize("//", "//", "");
    ExpectJavaDefaultNormalize("--123", "--123", "");
    ExpectJavaDefaultNormalize("//123", "//123", "");
    ExpectJavaDefaultNormalize("--test", "--test", "");
    ExpectJavaDefaultNormalize("//test", "//test", "");
    ExpectJavaDefaultNormalize("--test\ntest", "--test\ntest", "");
    ExpectJavaDefaultNormalize("--test\t\n", "--test\t\n", "");
    ExpectJavaCombine("--test\n123 test", "--test\n0# test", "123");
}

TEST_F(SqlTest, JavaDefaultMultiLineCommentCases) {
    ExpectJavaDefaultNormalize("/**/", "/**/", "");
    ExpectJavaDefaultNormalize("/* */", "/* */", "");
    ExpectJavaDefaultNormalize("/* */abc", "/* */abc", "");
    ExpectJavaDefaultNormalize("/* * */", "/* * */", "");
    ExpectJavaDefaultNormalize("/* abc", "/* abc", "");
    ExpectJavaDefaultNormalize("select * from table", "select * from table", "");

    ExpectJavaDefaultNormalize("/*", "/*", "");
    ExpectJavaDefaultNormalize("/*  ", "/*  ", "");
    ExpectJavaDefaultNormalize("/*  \n  ", "/*  \n  ", "");
}

TEST_F(SqlTest, JavaDefaultSymbolStateCases) {
    ExpectJavaDefaultNormalize("''", "''", "");
    ExpectJavaCombine("'abc'", "'0$'", "abc");
    ExpectJavaCombine("'a''bc'", "'0$'", "a''bc");
    ExpectJavaCombine("'a' 'bc'", "'0$' '1$'", "a,bc");
    ExpectJavaCombine("'a''bc' 'a''bc'", "'0$' '1$'", "a''bc,a''bc");
    ExpectJavaCombine("select * from table where a='a'", "select * from table where a='0$'", "a");
}

TEST_F(SqlTest, JavaDefaultCommentAndSymbolCases) {
    ExpectJavaDefaultNormalize("/* 'test' */", "/* 'test' */", "");
    ExpectJavaDefaultNormalize("/* 'test'' */", "/* 'test'' */", "");
    ExpectJavaDefaultNormalize("/* '' */", "/* '' */", "");
    ExpectJavaCombine("/*  */ 123 */", "/*  */ 0# */", "123");
    ExpectJavaCombine("' /* */'", "'0$'", " /* */");
}

TEST_F(SqlTest, JavaDefaultSeparatorCases) {
    ExpectJavaCombine("1234 456,7", "0# 1#,2#", "1234,456,7");
    ExpectJavaCombine("'1234 456,7'", "'0$'", "1234 456,,7");
    ExpectJavaCombine("'1234''456,7'", "'0$'", "1234''456,,7");
    ExpectJavaCombine("'1234' '456,7'", "'0$' '1$'", "1234,456,,7");
}

TEST_F(SqlTest, JavaDefaultEmptyStringCases) {
    ExpectJavaCombine(
        "select u.user_no as userNo,ifnull(s.equipment,'') as equipment,ifnull(s.gender, '0') as gender from user u left join supply s on u.user_no = s.user_no where u.user_no = ?",
        "select u.user_no as userNo,ifnull(s.equipment,'') as equipment,ifnull(s.gender, '0$') as gender from user u left join supply s on u.user_no = s.user_no where u.user_no = ?",
        "0");
    ExpectJavaCombine(
        "select u.user_no as userNo,ifnull(s.equipment,'test_str') as equipment,ifnull(s.gender, '0') as gender from user u left join supply s on u.user_no = s.user_no where u.user_no != ''",
        "select u.user_no as userNo,ifnull(s.equipment,'0$') as equipment,ifnull(s.gender, '1$') as gender from user u left join supply s on u.user_no = s.user_no where u.user_no != ''",
        "test_str,0");
    ExpectJavaCombine(
        "select concat ('hello,', u.name, ?)as hello, u.user_no as userNo from user u where 1 = 1 and u.user_no = '10010'",
        "select concat ('0$', u.name, ?)as hello, u.user_no as userNo from user u where 1# = 2# and u.user_no = '3$'",
        "hello,,,1,1,10010");
    ExpectJavaDefaultNormalize(
        "select concat ('hello,', u.name, ' ')as hello, u.user_no as userNo from user u where 1 = 1 and u.user_no != ''",
        "select concat ('0$', u.name, '1$')as hello, u.user_no as userNo from user u where 2# = 3# and u.user_no != ''",
        "hello,,, ,1,1");
    ExpectJavaCombine(
        "select concat ('hello,', u.name, 'zhangsan')as hello, u.user_no as userNo from user u where 1 = 1 and u.user_no != '' and u.age > 20",
        "select concat ('0$', u.name, '1$')as hello, u.user_no as userNo from user u where 2# = 3# and u.user_no != '' and u.age > 4#",
        "hello,,,zhangsan,1,1,20");
    ExpectJavaCombine(
        "select concat ('pinpoint,', u.name, (select s.user_no from user s where s.user_no = '8888'))as hello, u.user_no as userNo from user u where 1 = 1 and u.habit != '2768' and u.age > 20",
        "select concat ('0$', u.name, (select s.user_no from user s where s.user_no = '1$'))as hello, u.user_no as userNo from user u where 2# = 3# and u.habit != '4$' and u.age > 5#",
        "pinpoint,,,8888,1,1,2768,20");
    ExpectJavaCombine(
        "SELECT n.order_logistics_id, MAX(IF(IFNULL(n.id, '') != '', '2', '0')) AS is_ts FROM t_e_shipping_note n WHERE IFNULL(n.delflag, '') <> '1' AND IFNULL(n.document_require, '0') = '2' GROUP BY n.order_logistics_id",
        "SELECT n.order_logistics_id, MAX(IF(IFNULL(n.id, '') != '', '0$', '1$')) AS is_ts FROM t_e_shipping_note n WHERE IFNULL(n.delflag, '') <> '2$' AND IFNULL(n.document_require, '3$') = '4$' GROUP BY n.order_logistics_id",
        "2,0,1,0,2");
}

TEST_F(SqlTest, JavaRemoveCommentsCases) {
    SqlNormalizer remove_comments(2048, true);
    ExpectNormalize(remove_comments, "/** comment ? */ select * from table id = 'foo'",
        " select * from table id = '0$'", "foo");
    ExpectNormalize(remove_comments, "//comment\nselect * from table id = ?;",
        "select * from table id = ?;", "");
    ExpectNormalize(remove_comments, "--comment\nselect * from table id = ?;",
        "select * from table id = ?;", "");
    ExpectNormalize(remove_comments, "select */*comment*/ \nfrom table id = ?;",
        "select * \nfrom table id = ?;", "");
    ExpectNormalize(remove_comments, "select * from table id = ?; /* This ? comment*/",
        "select * from table id = ?; ", "");
    ExpectNormalize(remove_comments, "select * from table id = ?; // This ? comment",
        "select * from table id = ?; ", "");
    ExpectNormalize(remove_comments, "select * from table id = ?; -- This ? comment",
        "select * from table id = ?; ", "");
}

TEST_F(SqlTest, JavaPostgresPositionalParameterCases) {
    ExpectJavaCombine("SELECT * FROM member WHERE user = 'Kim' AND id = $1 AND no = 10",
        "SELECT * FROM member WHERE user = '0$' AND id = $1 AND no = 1#", "Kim,10");
    ExpectJavaCombine("SELECT * FROM member WHERE id = $122309 AND no = 122309",
        "SELECT * FROM member WHERE id = $122309 AND no = 0#", "122309");
    ExpectJavaCombine("$value, 123", "$value, 0#", "123");
    ExpectJavaCombine("'$123', 123", "'0$', 1#", "$123,123");
    ExpectJavaCombine("$; 123", "$; 0#", "123");
    ExpectJavaCombine("$(123); 123", "$(0#); 1#", "123,123");
    ExpectJavaCombine("'$''123'", "'0$'", "$''123");
}

} // namespace pinpoint
