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
#include <string>

namespace pinpoint {

class SqlTest : public ::testing::Test {
protected:
    void SetUp() override {
        normalizer_ = std::make_unique<SqlNormalizer>();
    }

    void TearDown() override {
        normalizer_.reset();
    }

protected:
    std::unique_ptr<SqlNormalizer> normalizer_;
};

// ========== Basic Normalization Tests ==========

// Test empty SQL
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
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE balance = 0#");
    EXPECT_EQ(result.parameters, "-100.50");
}

// Test decimal numbers
TEST_F(SqlTest, DecimalNumberParameterTest) {
    auto result = normalizer_->normalize("SELECT * FROM products WHERE price = 99.99");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM products WHERE price = 0#");
    EXPECT_EQ(result.parameters, "99.99");
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

// Test line comment removal
TEST_F(SqlTest, LineCommentRemovalTest) {
    auto result = normalizer_->normalize("SELECT * FROM users -- This is a comment");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users ");
    EXPECT_EQ(result.parameters, "");
    
    result = normalizer_->normalize("SELECT * FROM users -- Comment\nWHERE id = 1");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users \nWHERE id = 0#");
    EXPECT_EQ(result.parameters, "1");
}

// Test block comment removal
TEST_F(SqlTest, BlockCommentRemovalTest) {
    auto result = normalizer_->normalize("SELECT * /* This is a block comment */ FROM users");
    EXPECT_EQ(result.normalized_sql, "SELECT *   FROM users");
    EXPECT_EQ(result.parameters, "");
    
    result = normalizer_->normalize("SELECT * /* Multi\nline\ncomment */ FROM users");
    EXPECT_EQ(result.normalized_sql, "SELECT *   FROM users");
    EXPECT_EQ(result.parameters, "");
}

// Test mixed comments
TEST_F(SqlTest, MixedCommentsTest) {
    auto result = normalizer_->normalize("SELECT * /* block */ FROM users -- line comment");
    EXPECT_EQ(result.normalized_sql, "SELECT *   FROM users ");
    EXPECT_EQ(result.parameters, "");
}

// Test comments with parameters
TEST_F(SqlTest, CommentsWithParametersTest) {
    auto result = normalizer_->normalize("SELECT * FROM users /* ignore 123 */ WHERE id = 456 -- ignore :param");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users   WHERE id = 0# ");
    EXPECT_EQ(result.parameters, "456");
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
    EXPECT_EQ(result.parameters, "John's Company");
    
    result = normalizer_->normalize("SELECT * FROM users WHERE name = \"John's Company\"");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE name = \"0$\"");
    EXPECT_EQ(result.parameters, "John's Company");
    
    result = normalizer_->normalize("SELECT * FROM users WHERE name = `user_name`");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE name = `0$`");
    EXPECT_EQ(result.parameters, "user_name");
}

// Test string literals with escaped quotes (backslash escape not handled, so string ends at backslash)
TEST_F(SqlTest, EscapedQuotesTest) {
    auto result = normalizer_->normalize("SELECT * FROM users WHERE name = 'John\\'s Company'");
    EXPECT_EQ(result.normalized_sql, "SELECT * FROM users WHERE name = '0$'s Company");
    EXPECT_EQ(result.parameters, "John\\");
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

// Test SQL with only comments
TEST_F(SqlTest, OnlyCommentsTest) {
    auto result = normalizer_->normalize("/* This is only a comment */");
    EXPECT_EQ(result.normalized_sql, " ");
    EXPECT_EQ(result.parameters, "");
}

// Test SQL with malformed quotes
TEST_F(SqlTest, MalformedQuotesTest) {
    auto result = normalizer_->normalize("SELECT * FROM users WHERE name = 'unclosed quote AND id = 123");
    // Should still work even with malformed quotes
    EXPECT_FALSE(result.normalized_sql.empty());
    EXPECT_TRUE(result.normalized_sql.find("SELECT * FROM users") != std::string::npos);
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

// Test parameter indexing accuracy
TEST_F(SqlTest, ParameterIndexingAccuracyTest) {
    auto result = normalizer_->normalize("SELECT 1, 'literal', 2, 'another', 3.14, 42");
    EXPECT_EQ(result.normalized_sql, "SELECT 0#, '1$', 2#, '3$', 4#, 5#");
    EXPECT_EQ(result.parameters, "1,literal,2,another,3.14,42");
}

// Test nested quotes and comments
TEST_F(SqlTest, NestedQuotesAndCommentsTest) {
    auto result = normalizer_->normalize("SELECT 'Comment /* not really */' FROM table WHERE id = 123 -- 'not a string'");
    EXPECT_EQ(result.normalized_sql, "SELECT '0$' FROM table WHERE id = 1# ");
    EXPECT_EQ(result.parameters, "Comment /* not really */,123");
}

} // namespace pinpoint
