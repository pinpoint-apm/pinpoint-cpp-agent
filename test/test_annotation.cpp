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

#include "../src/annotation.h"
#include <gtest/gtest.h>
#include <string>
#include <list>
#include <memory>

namespace pinpoint {

class AnnotationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Called before each test
        annotation = std::make_unique<PinpointAnnotation>();
    }

    void TearDown() override {
        // Called after each test
        annotation.reset();
    }

protected:
    std::unique_ptr<PinpointAnnotation> annotation;
};

// ========== AppendInt Tests ==========

// Test AppendInt with positive integer
TEST_F(AnnotationTest, AppendIntPositiveTest) {
    int32_t key = 100;
    int value = 42;
    
    annotation->AppendInt(key, value);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 0) << "DataType should be 0 for int";
    EXPECT_EQ(pair.second->data.intValue, value) << "Int value should match";
}

// Test AppendInt with negative integer
TEST_F(AnnotationTest, AppendIntNegativeTest) {
    int32_t key = 101;
    int value = -123;
    
    annotation->AppendInt(key, value);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 0) << "DataType should be 0 for int";
    EXPECT_EQ(pair.second->data.intValue, value) << "Negative int value should match";
}

// Test AppendInt with zero
TEST_F(AnnotationTest, AppendIntZeroTest) {
    int32_t key = 102;
    int value = 0;
    
    annotation->AppendInt(key, value);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 0) << "DataType should be 0 for int";
    EXPECT_EQ(pair.second->data.intValue, value) << "Zero value should match";
}

// Test AppendInt with extreme values
TEST_F(AnnotationTest, AppendIntExtremeValuesTest) {
    annotation->AppendInt(200, INT32_MAX);
    annotation->AppendInt(201, INT32_MIN);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 2) << "Should have exactly 2 annotations";
    
    auto it = annotations.begin();
    EXPECT_EQ(it->first, 200) << "First key should match";
    EXPECT_EQ(it->second->data.intValue, INT32_MAX) << "INT32_MAX should match";
    
    ++it;
    EXPECT_EQ(it->first, 201) << "Second key should match";
    EXPECT_EQ(it->second->data.intValue, INT32_MIN) << "INT32_MIN should match";
}

// ========== AppendString Tests ==========

// Test AppendString with normal string
TEST_F(AnnotationTest, AppendStringNormalTest) {
    int32_t key = 300;
    std::string value = "Hello, World!";
    
    annotation->AppendString(key, value);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 1) << "DataType should be 1 for string";
    EXPECT_EQ(pair.second->data.stringValue, value) << "String value should match";
}

// Test AppendString with empty string
TEST_F(AnnotationTest, AppendStringEmptyTest) {
    int32_t key = 301;
    std::string value = "";
    
    annotation->AppendString(key, value);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 1) << "DataType should be 1 for string";
    EXPECT_EQ(pair.second->data.stringValue, value) << "Empty string should match";
}

// Test AppendString with special characters
TEST_F(AnnotationTest, AppendStringSpecialCharsTest) {
    int32_t key = 302;
    std::string value = "Special chars: !@#$%^&*()_+[]{}|;':\",./<>?\\`~\n\t\r";
    
    annotation->AppendString(key, value);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 1) << "DataType should be 1 for string";
    EXPECT_EQ(pair.second->data.stringValue, value) << "Special characters string should match";
}

// Test AppendString with Unicode characters
TEST_F(AnnotationTest, AppendStringUnicodeTest) {
    int32_t key = 303;
    std::string value = "Unicode: 한글, 日本語, العربية, 中文, Ελληνικά";
    
    annotation->AppendString(key, value);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 1) << "DataType should be 1 for string";
    EXPECT_EQ(pair.second->data.stringValue, value) << "Unicode string should match";
}

// ========== AppendStringString Tests ==========

// Test AppendStringString with normal strings
TEST_F(AnnotationTest, AppendStringStringNormalTest) {
    int32_t key = 400;
    std::string value1 = "First String";
    std::string value2 = "Second String";
    
    annotation->AppendStringString(key, value1, value2);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 2) << "DataType should be 2 for string-string";
    EXPECT_EQ(pair.second->data.stringStringValue.stringValue1, value1) << "First string should match";
    EXPECT_EQ(pair.second->data.stringStringValue.stringValue2, value2) << "Second string should match";
}

// Test AppendStringString with empty strings
TEST_F(AnnotationTest, AppendStringStringEmptyTest) {
    int32_t key = 401;
    std::string value1 = "";
    std::string value2 = "";
    
    annotation->AppendStringString(key, value1, value2);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 2) << "DataType should be 2 for string-string";
    EXPECT_EQ(pair.second->data.stringStringValue.stringValue1, value1) << "First empty string should match";
    EXPECT_EQ(pair.second->data.stringStringValue.stringValue2, value2) << "Second empty string should match";
}

// Test AppendStringString with mixed content
TEST_F(AnnotationTest, AppendStringStringMixedTest) {
    int32_t key = 402;
    std::string value1 = "Content-Type";
    std::string value2 = "application/json; charset=utf-8";
    
    annotation->AppendStringString(key, value1, value2);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 2) << "DataType should be 2 for string-string";
    EXPECT_EQ(pair.second->data.stringStringValue.stringValue1, value1) << "Header name should match";
    EXPECT_EQ(pair.second->data.stringStringValue.stringValue2, value2) << "Header value should match";
}

// ========== AppendIntStringString Tests ==========

// Test AppendIntStringString with normal values
TEST_F(AnnotationTest, AppendIntStringStringNormalTest) {
    int32_t key = 500;
    int intValue = 42;
    std::string string1 = "Method";
    std::string string2 = "GET";
    
    annotation->AppendIntStringString(key, intValue, string1, string2);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 3) << "DataType should be 3 for int-string-string";
    EXPECT_EQ(pair.second->data.intStringStringValue.intValue, intValue) << "Int value should match";
    EXPECT_EQ(pair.second->data.intStringStringValue.stringValue1, string1) << "First string should match";
    EXPECT_EQ(pair.second->data.intStringStringValue.stringValue2, string2) << "Second string should match";
}

// Test AppendIntStringString with negative int and empty strings
TEST_F(AnnotationTest, AppendIntStringStringEdgeCaseTest) {
    int32_t key = 501;
    int intValue = -999;
    std::string string1 = "";
    std::string string2 = "Non-empty";
    
    annotation->AppendIntStringString(key, intValue, string1, string2);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 3) << "DataType should be 3 for int-string-string";
    EXPECT_EQ(pair.second->data.intStringStringValue.intValue, intValue) << "Negative int value should match";
    EXPECT_EQ(pair.second->data.intStringStringValue.stringValue1, string1) << "Empty string should match";
    EXPECT_EQ(pair.second->data.intStringStringValue.stringValue2, string2) << "Non-empty string should match";
}

// ========== AppendLongIntIntByteByteString Tests ==========

// Test AppendLongIntIntByteByteString with normal values
TEST_F(AnnotationTest, AppendLongIntIntByteByteStringNormalTest) {
    int32_t key = 600;
    int64_t longValue = 1234567890123456789LL;
    int32_t int1 = 42;
    int32_t int2 = 84;
    int32_t byte1 = 0xFF;
    int32_t byte2 = 0x00;
    std::string stringValue = "Complex annotation data";
    
    annotation->AppendLongIntIntByteByteString(key, longValue, int1, int2, byte1, byte2, stringValue);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 4) << "DataType should be 4 for long-int-int-byte-byte-string";
    
    auto& complexData = pair.second->data.longIntIntByteByteStringValue;
    EXPECT_EQ(complexData.longValue, longValue) << "Long value should match";
    EXPECT_EQ(complexData.intValue1, int1) << "First int value should match";
    EXPECT_EQ(complexData.intValue2, int2) << "Second int value should match";
    EXPECT_EQ(complexData.byteValue1, byte1) << "First byte value should match";
    EXPECT_EQ(complexData.byteValue2, byte2) << "Second byte value should match";
    EXPECT_EQ(complexData.stringValue, stringValue) << "String value should match";
}

// Test AppendLongIntIntByteByteString with extreme values
TEST_F(AnnotationTest, AppendLongIntIntByteByteStringExtremeTest) {
    int32_t key = 601;
    int64_t longValue = INT64_MIN;
    int32_t int1 = INT32_MAX;
    int32_t int2 = INT32_MIN;
    int32_t byte1 = 0x00;
    int32_t byte2 = 0xFF;
    std::string stringValue = "";
    
    annotation->AppendLongIntIntByteByteString(key, longValue, int1, int2, byte1, byte2, stringValue);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 4) << "DataType should be 4 for long-int-int-byte-byte-string";
    
    auto& complexData = pair.second->data.longIntIntByteByteStringValue;
    EXPECT_EQ(complexData.longValue, longValue) << "Extreme long value should match";
    EXPECT_EQ(complexData.intValue1, int1) << "Extreme int1 value should match";
    EXPECT_EQ(complexData.intValue2, int2) << "Extreme int2 value should match";
    EXPECT_EQ(complexData.byteValue1, byte1) << "Byte1 value should match";
    EXPECT_EQ(complexData.byteValue2, byte2) << "Byte2 value should match";
    EXPECT_EQ(complexData.stringValue, stringValue) << "Empty string should match";
}

// ========== Multiple Annotations Tests ==========

// Test adding multiple annotations of different types
TEST_F(AnnotationTest, MultipleAnnotationTypesTest) {
    annotation->AppendInt(1, 42);
    annotation->AppendString(2, "Test String");
    annotation->AppendStringString(3, "Key", "Value");
    annotation->AppendIntStringString(4, 100, "Method", "POST");
    annotation->AppendLongIntIntByteByteString(5, 123456789LL, 1, 2, 3, 4, "Complex");
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 5) << "Should have exactly 5 annotations";
    
    auto it = annotations.begin();
    
    // Check first annotation (Int)
    EXPECT_EQ(it->first, 1) << "First annotation key should match";
    EXPECT_EQ(it->second->dataType, 0) << "First annotation should be int type";
    EXPECT_EQ(it->second->data.intValue, 42) << "First annotation value should match";
    
    // Check second annotation (String)
    ++it;
    EXPECT_EQ(it->first, 2) << "Second annotation key should match";
    EXPECT_EQ(it->second->dataType, 1) << "Second annotation should be string type";
    EXPECT_EQ(it->second->data.stringValue, "Test String") << "Second annotation value should match";
    
    // Check third annotation (StringString)
    ++it;
    EXPECT_EQ(it->first, 3) << "Third annotation key should match";
    EXPECT_EQ(it->second->dataType, 2) << "Third annotation should be string-string type";
    EXPECT_EQ(it->second->data.stringStringValue.stringValue1, "Key") << "Third annotation first string should match";
    EXPECT_EQ(it->second->data.stringStringValue.stringValue2, "Value") << "Third annotation second string should match";
    
    // Check fourth annotation (IntStringString)
    ++it;
    EXPECT_EQ(it->first, 4) << "Fourth annotation key should match";
    EXPECT_EQ(it->second->dataType, 3) << "Fourth annotation should be int-string-string type";
    EXPECT_EQ(it->second->data.intStringStringValue.intValue, 100) << "Fourth annotation int should match";
    EXPECT_EQ(it->second->data.intStringStringValue.stringValue1, "Method") << "Fourth annotation first string should match";
    EXPECT_EQ(it->second->data.intStringStringValue.stringValue2, "POST") << "Fourth annotation second string should match";
    
    // Check fifth annotation (Complex)
    ++it;
    EXPECT_EQ(it->first, 5) << "Fifth annotation key should match";
    EXPECT_EQ(it->second->dataType, 4) << "Fifth annotation should be complex type";
    auto& complexData = it->second->data.longIntIntByteByteStringValue;
    EXPECT_EQ(complexData.longValue, 123456789LL) << "Fifth annotation long should match";
    EXPECT_EQ(complexData.intValue1, 1) << "Fifth annotation int1 should match";
    EXPECT_EQ(complexData.intValue2, 2) << "Fifth annotation int2 should match";
    EXPECT_EQ(complexData.byteValue1, 3) << "Fifth annotation byte1 should match";
    EXPECT_EQ(complexData.byteValue2, 4) << "Fifth annotation byte2 should match";
    EXPECT_EQ(complexData.stringValue, "Complex") << "Fifth annotation string should match";
}

// Test insertion order preservation
TEST_F(AnnotationTest, InsertionOrderTest) {
    // Add annotations in specific order
    annotation->AppendInt(100, 1);
    annotation->AppendInt(200, 2);
    annotation->AppendInt(300, 3);
    annotation->AppendInt(400, 4);
    annotation->AppendInt(500, 5);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 5) << "Should have exactly 5 annotations";
    
    int expectedKey = 100;
    int expectedValue = 1;
    
    for (const auto& pair : annotations) {
        EXPECT_EQ(pair.first, expectedKey) << "Key should maintain insertion order";
        EXPECT_EQ(pair.second->data.intValue, expectedValue) << "Value should maintain insertion order";
        expectedKey += 100;
        expectedValue += 1;
    }
}

// Test same key multiple times
TEST_F(AnnotationTest, SameKeyMultipleTimesTest) {
    int32_t key = 999;
    annotation->AppendInt(key, 1);
    annotation->AppendInt(key, 2);
    annotation->AppendString(key, "Three");
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 3) << "Should have exactly 3 annotations with same key";
    
    auto it = annotations.begin();
    EXPECT_EQ(it->first, key) << "First annotation key should match";
    EXPECT_EQ(it->second->dataType, 0) << "First annotation should be int type";
    EXPECT_EQ(it->second->data.intValue, 1) << "First annotation value should be 1";
    
    ++it;
    EXPECT_EQ(it->first, key) << "Second annotation key should match";
    EXPECT_EQ(it->second->dataType, 0) << "Second annotation should be int type";
    EXPECT_EQ(it->second->data.intValue, 2) << "Second annotation value should be 2";
    
    ++it;
    EXPECT_EQ(it->first, key) << "Third annotation key should match";
    EXPECT_EQ(it->second->dataType, 1) << "Third annotation should be string type";
    EXPECT_EQ(it->second->data.stringValue, "Three") << "Third annotation value should be 'Three'";
}

// ========== Edge Cases and Boundary Tests ==========

// Test very long strings
TEST_F(AnnotationTest, VeryLongStringTest) {
    int32_t key = 700;
    std::string longString(10000, 'A'); // 10,000 character string
    
    annotation->AppendString(key, longString);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 1) << "DataType should be 1 for string";
    EXPECT_EQ(pair.second->data.stringValue, longString) << "Very long string should match";
    EXPECT_EQ(pair.second->data.stringValue.length(), 10000) << "String length should be 10000";
}

// Test null characters in string
TEST_F(AnnotationTest, NullCharactersInStringTest) {
    int32_t key = 701;
    std::string stringWithNulls = std::string("Hello\0World\0Test", 17);
    
    annotation->AppendString(key, stringWithNulls);
    
    auto& annotations = annotation->getAnnotations();
    EXPECT_EQ(annotations.size(), 1) << "Should have exactly 1 annotation";
    
    auto& pair = annotations.front();
    EXPECT_EQ(pair.first, key) << "Key should match";
    EXPECT_EQ(pair.second->dataType, 1) << "DataType should be 1 for string";
    EXPECT_EQ(pair.second->data.stringValue, stringWithNulls) << "String with null characters should match";
    EXPECT_EQ(pair.second->data.stringValue.length(), 17) << "String length should include null characters";
}

// Test empty annotation list
TEST_F(AnnotationTest, EmptyAnnotationListTest) {
    auto& annotations = annotation->getAnnotations();
    EXPECT_TRUE(annotations.empty()) << "New annotation should have empty list";
    EXPECT_EQ(annotations.size(), 0) << "Size should be 0";
}

} // namespace pinpoint
