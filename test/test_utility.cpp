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

#include "../src/utility.h"
#include <gtest/gtest.h>
#include <set>
#include <thread>
#include <vector>
#include <cmath>

namespace pinpoint {

// ========== generate_span_id Tests ==========

TEST(UtilityTest, GenerateSpanIdReturnsNonZero) {
    // While technically 0 is possible, it's astronomically unlikely
    bool found_non_zero = false;
    for (int i = 0; i < 10; ++i) {
        if (generate_span_id() != 0) {
            found_non_zero = true;
            break;
        }
    }
    EXPECT_TRUE(found_non_zero);
}

TEST(UtilityTest, GenerateSpanIdProducesUniqueValues) {
    std::set<int64_t> ids;
    constexpr int count = 1000;
    for (int i = 0; i < count; ++i) {
        ids.insert(generate_span_id());
    }
    // With 64-bit random values, collisions in 1000 samples are essentially impossible
    EXPECT_EQ(ids.size(), count);
}

TEST(UtilityTest, GenerateSpanIdThreadSafety) {
    constexpr int threads_count = 4;
    constexpr int ids_per_thread = 250;
    std::vector<std::vector<int64_t>> thread_ids(threads_count);
    std::vector<std::thread> threads;

    for (int t = 0; t < threads_count; ++t) {
        threads.emplace_back([&thread_ids, t]() {
            for (int i = 0; i < ids_per_thread; ++i) {
                thread_ids[t].push_back(generate_span_id());
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    std::set<int64_t> all_ids;
    for (const auto& ids : thread_ids) {
        all_ids.insert(ids.begin(), ids.end());
    }
    EXPECT_EQ(all_ids.size(), threads_count * ids_per_thread);
}

// ========== to_milli_seconds Tests ==========

TEST(UtilityTest, ToMilliSecondsEpoch) {
    auto epoch = std::chrono::system_clock::time_point{};
    EXPECT_EQ(to_milli_seconds(epoch), 0);
}

TEST(UtilityTest, ToMilliSecondsKnownValue) {
    auto epoch = std::chrono::system_clock::time_point{};
    auto tp = epoch + std::chrono::milliseconds(1234567890);
    EXPECT_EQ(to_milli_seconds(tp), 1234567890);
}

TEST(UtilityTest, ToMilliSecondsNow) {
    auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto result = to_milli_seconds(std::chrono::system_clock::now());
    auto after = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    EXPECT_GE(result, before);
    EXPECT_LE(result, after);
}

// ========== get_host_name Tests ==========

TEST(UtilityTest, GetHostNameReturnsNonEmpty) {
    auto name = get_host_name();
    EXPECT_FALSE(name.empty());
    EXPECT_NE(name, "unknown");
}

// ========== get_host_ip_addr Tests ==========

TEST(UtilityTest, GetHostIpAddrReturnsValidFormat) {
    auto ip = get_host_ip_addr();
    EXPECT_FALSE(ip.empty());
    // Should contain dots (IPv4 format)
    EXPECT_NE(ip.find('.'), std::string::npos);
}

// ========== compare_string Tests ==========

TEST(UtilityTest, CompareStringExactMatch) {
    EXPECT_TRUE(compare_string("hello", "hello"));
}

TEST(UtilityTest, CompareStringCaseInsensitive) {
    EXPECT_TRUE(compare_string("Hello", "hello"));
    EXPECT_TRUE(compare_string("HELLO", "hello"));
    EXPECT_TRUE(compare_string("HeLLo", "hEllO"));
}

TEST(UtilityTest, CompareStringDifferentStrings) {
    EXPECT_FALSE(compare_string("hello", "world"));
    EXPECT_FALSE(compare_string("hello", "hell"));
}

TEST(UtilityTest, CompareStringDifferentLengths) {
    EXPECT_FALSE(compare_string("hello", "helloo"));
    EXPECT_FALSE(compare_string("", "a"));
    EXPECT_FALSE(compare_string("a", ""));
}

TEST(UtilityTest, CompareStringEmpty) {
    EXPECT_TRUE(compare_string("", ""));
}

TEST(UtilityTest, CompareStringSpecialChars) {
    EXPECT_TRUE(compare_string("hello-world_123", "hello-world_123"));
    EXPECT_TRUE(compare_string("Hello-World_123", "hello-world_123"));
    // Non-alpha chars must match exactly
    EXPECT_FALSE(compare_string("hello-world", "hello_world"));
}

// ========== stoi_ Tests ==========

TEST(UtilityTest, StoiValidIntegers) {
    EXPECT_EQ(stoi_("0").value(), 0);
    EXPECT_EQ(stoi_("42").value(), 42);
    EXPECT_EQ(stoi_("-100").value(), -100);
    EXPECT_EQ(stoi_("2147483647").value(), 2147483647);  // INT_MAX
}

TEST(UtilityTest, StoiInvalidInput) {
    EXPECT_FALSE(stoi_("").has_value());
    EXPECT_FALSE(stoi_("abc").has_value());
    EXPECT_FALSE(stoi_("12.34").has_value());
    EXPECT_FALSE(stoi_("12abc").has_value());
}

// ========== stoll_ Tests ==========

TEST(UtilityTest, StollValidIntegers) {
    EXPECT_EQ(stoll_("0").value(), 0LL);
    EXPECT_EQ(stoll_("9999999999").value(), 9999999999LL);
    EXPECT_EQ(stoll_("-9999999999").value(), -9999999999LL);
}

TEST(UtilityTest, StollInvalidInput) {
    EXPECT_FALSE(stoll_("").has_value());
    EXPECT_FALSE(stoll_("not_a_number").has_value());
    EXPECT_FALSE(stoll_("1.5").has_value());
}

// ========== stod_ Tests ==========

TEST(UtilityTest, StodValidDoubles) {
    EXPECT_DOUBLE_EQ(stod_("0.0").value(), 0.0);
    EXPECT_DOUBLE_EQ(stod_("3.14").value(), 3.14);
    EXPECT_DOUBLE_EQ(stod_("-2.5").value(), -2.5);
    EXPECT_DOUBLE_EQ(stod_("100").value(), 100.0);
}

TEST(UtilityTest, StodInvalidInput) {
    EXPECT_FALSE(stod_("").has_value());
    EXPECT_FALSE(stod_("abc").has_value());
}

// ========== stob_ Tests ==========

TEST(UtilityTest, StobTrueValues) {
    EXPECT_TRUE(stob_("true").value());
    EXPECT_TRUE(stob_("1").value());
}

TEST(UtilityTest, StobFalseValues) {
    EXPECT_FALSE(stob_("false").value());
    EXPECT_FALSE(stob_("0").value());
}

TEST(UtilityTest, StobInvalidInput) {
    EXPECT_FALSE(stob_("").has_value());
    EXPECT_FALSE(stob_("maybe").has_value());
    EXPECT_FALSE(stob_("2").has_value());
}

// absl::SimpleAtob accepts "yes"/"no", "y"/"n", "t"/"f" in addition to "true"/"false"/"1"/"0"
TEST(UtilityTest, StobAbslExtendedValues) {
    EXPECT_TRUE(stob_("yes").has_value());
    EXPECT_TRUE(stob_("yes").value());
    EXPECT_TRUE(stob_("no").has_value());
    EXPECT_FALSE(stob_("no").value());
    EXPECT_TRUE(stob_("y").has_value());
    EXPECT_TRUE(stob_("y").value());
    EXPECT_TRUE(stob_("n").has_value());
    EXPECT_FALSE(stob_("n").value());
}

// ========== generate_sql_uid Tests ==========

TEST(UtilityTest, GenerateSqlUidReturns16Bytes) {
    auto uid = generate_sql_uid("SELECT 1");
    EXPECT_EQ(uid.size(), 16u);
}

TEST(UtilityTest, GenerateSqlUidDeterministic) {
    auto uid1 = generate_sql_uid("SELECT * FROM users");
    auto uid2 = generate_sql_uid("SELECT * FROM users");
    EXPECT_EQ(uid1, uid2);
}

TEST(UtilityTest, GenerateSqlUidDifferentInputsDifferentOutput) {
    auto uid1 = generate_sql_uid("SELECT * FROM users");
    auto uid2 = generate_sql_uid("SELECT * FROM orders");
    EXPECT_NE(uid1, uid2);
}

TEST(UtilityTest, GenerateSqlUidEmptyString) {
    auto uid = generate_sql_uid("");
    EXPECT_EQ(uid.size(), 16u);
}

TEST(UtilityTest, GenerateSqlUidSimilarInputsDifferentOutput) {
    auto uid1 = generate_sql_uid("SELECT * FROM users WHERE id = 1");
    auto uid2 = generate_sql_uid("SELECT * FROM users WHERE id = 2");
    EXPECT_NE(uid1, uid2);
}

} // namespace pinpoint
