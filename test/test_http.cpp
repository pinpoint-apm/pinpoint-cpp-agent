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

#include "../src/http.h"
#include "../include/pinpoint/tracer.h"
#include <gtest/gtest.h>
#include <vector>
#include <string>
#include <map>
#include <cstring>
#include <sstream>
#include <functional>
#include <memory>

namespace pinpoint {

// Mock implementations for testing
class MockHeaderReader : public HeaderReader {
public:
    MockHeaderReader(const std::map<std::string, std::string>& headers) : headers_(headers) {}

    std::optional<std::string> Get(std::string_view key) const override {
        std::string keyStr(key.data(), key.size());
        auto it = headers_.find(keyStr);
        if (it != headers_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    void ForEach(std::function<bool(std::string_view key, std::string_view val)> callback) const override {
        for (const auto& pair : headers_) {
            if (!callback(pair.first, pair.second)) {
                break;
            }
        }
    }

private:
    std::map<std::string, std::string> headers_;
};

class MockAnnotation : public Annotation {
public:
    void AppendInt(int32_t key, int i) override {
        int_values_[key].push_back(i);
    }

    void AppendString(int32_t key, std::string_view s) override {
        string_values_[key].push_back(std::string(s));
    }

    void AppendStringString(int32_t key, std::string_view s1, std::string_view s2) override {
        string_string_values_[key].push_back(std::make_pair(std::string(s1), std::string(s2)));
    }

    void AppendIntStringString(int32_t key, int i, std::string_view s1, std::string_view s2) override {
        // Store as string for simplification
        string_values_[key].push_back(std::string(s1) + ":" + std::string(s2));
    }

    void AppendLongIntIntByteByteString(int32_t key, int64_t l, int32_t i1, int32_t i2, int32_t b1, int32_t b2, std::string_view s) override {
        // Store as string for simplification
        string_values_[key].push_back(std::string(s));
    }

    // Getters for verification
    const std::map<int32_t, std::vector<int> >& GetIntValues() const { return int_values_; }
    const std::map<int32_t, std::vector<std::string> >& GetStringValues() const { return string_values_; }
    const std::map<int32_t, std::vector<std::pair<std::string, std::string> > >& GetStringStringValues() const { return string_string_values_; }

    // Helper method to get total count of string-string annotations
    size_t GetStringStringCount() const {
        size_t total = 0;
        for (const auto& pair : string_string_values_) {
            total += pair.second.size();
        }
        return total;
    }

private:
    std::map<int32_t, std::vector<int> > int_values_;
    std::map<int32_t, std::vector<std::string> > string_values_;
    std::map<int32_t, std::vector<std::pair<std::string, std::string> > > string_string_values_;
};

// Utility functions needed by http.cpp
bool compare_string(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(a[i]) != std::tolower(b[i])) return false;
    }
    return true;
}

int stoi_(const std::string& str) {
    std::stringstream ss(str);
    int result;
    ss >> result;
    return result;
}

class HttpTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Called before each test
    }

    void TearDown() override {
        // Called after each test
    }
};

// ========== HTTP Status Code Classes Tests ==========

// Test HttpStatusDefault class
TEST_F(HttpTest, HttpStatusDefaultTest) {
    HttpStatusDefault status404(404);
    
    // Should match exact status code
    EXPECT_TRUE(status404.isError(404)) << "Should match exact status code 404";
    
    // Should not match different status codes
    EXPECT_FALSE(status404.isError(403)) << "Should not match different status code 403";
    EXPECT_FALSE(status404.isError(500)) << "Should not match different status code 500";
    EXPECT_FALSE(status404.isError(200)) << "Should not match different status code 200";
}

// Test HttpStatusInformational class (1xx)
TEST_F(HttpTest, HttpStatusInformationalTest) {
    HttpStatusInformational status;
    
    // Should match 1xx range
    EXPECT_TRUE(status.isError(100)) << "Should match status code 100";
    EXPECT_TRUE(status.isError(150)) << "Should match status code 150";
    EXPECT_TRUE(status.isError(199)) << "Should match status code 199";
    
    // Should not match outside 1xx range
    EXPECT_FALSE(status.isError(99)) << "Should not match status code 99";
    EXPECT_FALSE(status.isError(200)) << "Should not match status code 200";
    EXPECT_FALSE(status.isError(404)) << "Should not match status code 404";
}

// Test HttpStatusSuccess class (2xx)
TEST_F(HttpTest, HttpStatusSuccessTest) {
    HttpStatusSuccess status;
    
    // Should match 2xx range
    EXPECT_TRUE(status.isError(200)) << "Should match status code 200";
    EXPECT_TRUE(status.isError(250)) << "Should match status code 250";
    EXPECT_TRUE(status.isError(299)) << "Should match status code 299";
    
    // Should not match outside 2xx range
    EXPECT_FALSE(status.isError(199)) << "Should not match status code 199";
    EXPECT_FALSE(status.isError(300)) << "Should not match status code 300";
    EXPECT_FALSE(status.isError(404)) << "Should not match status code 404";
}

// Test HttpStatusRedirection class (3xx)
TEST_F(HttpTest, HttpStatusRedirectionTest) {
    HttpStatusRedirection status;
    
    // Should match 3xx range
    EXPECT_TRUE(status.isError(300)) << "Should match status code 300";
    EXPECT_TRUE(status.isError(350)) << "Should match status code 350";
    EXPECT_TRUE(status.isError(399)) << "Should match status code 399";
    
    // Should not match outside 3xx range
    EXPECT_FALSE(status.isError(299)) << "Should not match status code 299";
    EXPECT_FALSE(status.isError(400)) << "Should not match status code 400";
    EXPECT_FALSE(status.isError(200)) << "Should not match status code 200";
}

// Test HttpStatusClientError class (4xx)
TEST_F(HttpTest, HttpStatusClientErrorTest) {
    HttpStatusClientError status;
    
    // Should match 4xx range
    EXPECT_TRUE(status.isError(400)) << "Should match status code 400";
    EXPECT_TRUE(status.isError(404)) << "Should match status code 404";
    EXPECT_TRUE(status.isError(499)) << "Should match status code 499";
    
    // Should not match outside 4xx range
    EXPECT_FALSE(status.isError(399)) << "Should not match status code 399";
    EXPECT_FALSE(status.isError(500)) << "Should not match status code 500";
    EXPECT_FALSE(status.isError(200)) << "Should not match status code 200";
}

// Test HttpStatusServerError class (5xx)
TEST_F(HttpTest, HttpStatusServerErrorTest) {
    HttpStatusServerError status;
    
    // Should match 5xx range
    EXPECT_TRUE(status.isError(500)) << "Should match status code 500";
    EXPECT_TRUE(status.isError(550)) << "Should match status code 550";
    EXPECT_TRUE(status.isError(599)) << "Should match status code 599";
    
    // Should not match outside 5xx range
    EXPECT_FALSE(status.isError(499)) << "Should not match status code 499";
    EXPECT_FALSE(status.isError(600)) << "Should not match status code 600";
    EXPECT_FALSE(status.isError(200)) << "Should not match status code 200";
}

// ========== HttpStatusErrors Class Tests ==========

// Test HttpStatusErrors with single category
TEST_F(HttpTest, HttpStatusErrorsSingleCategoryTest) {
    std::vector<std::string> tokens = {"5xx"};
    HttpStatusErrors errors(tokens);
    
    // Should match server errors
    EXPECT_TRUE(errors.isErrorCode(500)) << "Should match 500 for 5xx category";
    EXPECT_TRUE(errors.isErrorCode(502)) << "Should match 502 for 5xx category";
    EXPECT_TRUE(errors.isErrorCode(599)) << "Should match 599 for 5xx category";
    
    // Should not match other categories
    EXPECT_FALSE(errors.isErrorCode(404)) << "Should not match 404 for 5xx category";
    EXPECT_FALSE(errors.isErrorCode(200)) << "Should not match 200 for 5xx category";
    EXPECT_FALSE(errors.isErrorCode(300)) << "Should not match 300 for 5xx category";
}

// Test HttpStatusErrors with multiple categories
TEST_F(HttpTest, HttpStatusErrorsMultipleCategoriesTest) {
    std::vector<std::string> tokens = {"4xx", "5xx"};
    HttpStatusErrors errors(tokens);
    
    // Should match both client and server errors
    EXPECT_TRUE(errors.isErrorCode(400)) << "Should match 400 for 4xx category";
    EXPECT_TRUE(errors.isErrorCode(404)) << "Should match 404 for 4xx category";
    EXPECT_TRUE(errors.isErrorCode(500)) << "Should match 500 for 5xx category";
    EXPECT_TRUE(errors.isErrorCode(502)) << "Should match 502 for 5xx category";
    
    // Should not match success or redirection
    EXPECT_FALSE(errors.isErrorCode(200)) << "Should not match 200";
    EXPECT_FALSE(errors.isErrorCode(301)) << "Should not match 301";
}

// Test HttpStatusErrors with specific codes
TEST_F(HttpTest, HttpStatusErrorsSpecificCodesTest) {
    std::vector<std::string> tokens = {"404", "503"};
    HttpStatusErrors errors(tokens);
    
    // Should match specific codes
    EXPECT_TRUE(errors.isErrorCode(404)) << "Should match specific code 404";
    EXPECT_TRUE(errors.isErrorCode(503)) << "Should match specific code 503";
    
    // Should not match other codes in same category
    EXPECT_FALSE(errors.isErrorCode(403)) << "Should not match 403 (other 4xx)";
    EXPECT_FALSE(errors.isErrorCode(500)) << "Should not match 500 (other 5xx)";
    EXPECT_FALSE(errors.isErrorCode(200)) << "Should not match 200";
}

// ========== Extended HttpStatusErrors Tests with Custom Arrays ==========

// Test with mixed categories and specific codes
TEST_F(HttpTest, HttpStatusErrorsCustomArray1Test) {
    std::vector<std::string> tokens = {"404", "500", "503", "2xx"};
    HttpStatusErrors errors(tokens);
    
    // Should match specific codes
    EXPECT_TRUE(errors.isErrorCode(404)) << "Should match specific code 404";
    EXPECT_TRUE(errors.isErrorCode(500)) << "Should match specific code 500";
    EXPECT_TRUE(errors.isErrorCode(503)) << "Should match specific code 503";
    
    // Should match 2xx category
    EXPECT_TRUE(errors.isErrorCode(200)) << "Should match 200 for 2xx category";
    EXPECT_TRUE(errors.isErrorCode(201)) << "Should match 201 for 2xx category";
    EXPECT_TRUE(errors.isErrorCode(299)) << "Should match 299 for 2xx category";
    
    // Should not match other codes
    EXPECT_FALSE(errors.isErrorCode(403)) << "Should not match 403 (other 4xx except 404)";
    EXPECT_FALSE(errors.isErrorCode(502)) << "Should not match 502 (other 5xx except 500, 503)";
    EXPECT_FALSE(errors.isErrorCode(301)) << "Should not match 301 (3xx not included)";
    EXPECT_FALSE(errors.isErrorCode(100)) << "Should not match 100 (1xx not included)";
}

// Test with all ranges and some specific codes
TEST_F(HttpTest, HttpStatusErrorsCustomArray2Test) {
    std::vector<std::string> tokens = {"1xx", "3xx", "4xx", "200", "502"};
    HttpStatusErrors errors(tokens);
    
    // Should match 1xx range
    EXPECT_TRUE(errors.isErrorCode(100)) << "Should match 100 for 1xx category";
    EXPECT_TRUE(errors.isErrorCode(150)) << "Should match 150 for 1xx category";
    EXPECT_TRUE(errors.isErrorCode(199)) << "Should match 199 for 1xx category";
    
    // Should match 3xx range
    EXPECT_TRUE(errors.isErrorCode(300)) << "Should match 300 for 3xx category";
    EXPECT_TRUE(errors.isErrorCode(350)) << "Should match 350 for 3xx category";
    EXPECT_TRUE(errors.isErrorCode(399)) << "Should match 399 for 3xx category";
    
    // Should match 4xx range
    EXPECT_TRUE(errors.isErrorCode(400)) << "Should match 400 for 4xx category";
    EXPECT_TRUE(errors.isErrorCode(404)) << "Should match 404 for 4xx category";
    EXPECT_TRUE(errors.isErrorCode(499)) << "Should match 499 for 4xx category";
    
    // Should match specific codes
    EXPECT_TRUE(errors.isErrorCode(200)) << "Should match specific code 200";
    EXPECT_TRUE(errors.isErrorCode(502)) << "Should match specific code 502";
    
    // Should not match other codes
    EXPECT_FALSE(errors.isErrorCode(201)) << "Should not match 201 (other 2xx except 200)";
    EXPECT_FALSE(errors.isErrorCode(500)) << "Should not match 500 (other 5xx except 502)";
    EXPECT_FALSE(errors.isErrorCode(600)) << "Should not match 600 (out of range)";
}

// Test with only specific codes
TEST_F(HttpTest, HttpStatusErrorsCustomArray3Test) {
    std::vector<std::string> tokens = {"201", "301", "401", "501"};
    HttpStatusErrors errors(tokens);
    
    // Should match only specific codes
    EXPECT_TRUE(errors.isErrorCode(201)) << "Should match specific code 201";
    EXPECT_TRUE(errors.isErrorCode(301)) << "Should match specific code 301";
    EXPECT_TRUE(errors.isErrorCode(401)) << "Should match specific code 401";
    EXPECT_TRUE(errors.isErrorCode(501)) << "Should match specific code 501";
    
    // Should not match related codes in same category
    EXPECT_FALSE(errors.isErrorCode(200)) << "Should not match 200 (other 2xx)";
    EXPECT_FALSE(errors.isErrorCode(202)) << "Should not match 202 (other 2xx)";
    EXPECT_FALSE(errors.isErrorCode(300)) << "Should not match 300 (other 3xx)";
    EXPECT_FALSE(errors.isErrorCode(302)) << "Should not match 302 (other 3xx)";
    EXPECT_FALSE(errors.isErrorCode(400)) << "Should not match 400 (other 4xx)";
    EXPECT_FALSE(errors.isErrorCode(402)) << "Should not match 402 (other 4xx)";
    EXPECT_FALSE(errors.isErrorCode(500)) << "Should not match 500 (other 5xx)";
    EXPECT_FALSE(errors.isErrorCode(502)) << "Should not match 502 (other 5xx)";
}

// Test edge cases and boundary values
TEST_F(HttpTest, HttpStatusErrorsCustomArray4Test) {
    std::vector<std::string> tokens = {"100", "199", "200", "299", "5xx"};
    HttpStatusErrors errors(tokens);
    
    // Should match boundary specific codes
    EXPECT_TRUE(errors.isErrorCode(100)) << "Should match boundary code 100";
    EXPECT_TRUE(errors.isErrorCode(199)) << "Should match boundary code 199";
    EXPECT_TRUE(errors.isErrorCode(200)) << "Should match boundary code 200";
    EXPECT_TRUE(errors.isErrorCode(299)) << "Should match boundary code 299";
    
    // Should match all 5xx
    EXPECT_TRUE(errors.isErrorCode(500)) << "Should match 500 for 5xx category";
    EXPECT_TRUE(errors.isErrorCode(550)) << "Should match 550 for 5xx category";
    EXPECT_TRUE(errors.isErrorCode(599)) << "Should match 599 for 5xx category";
    
    // Should not match adjacent codes
    EXPECT_FALSE(errors.isErrorCode(99)) << "Should not match 99 (before 100)";
    EXPECT_FALSE(errors.isErrorCode(101)) << "Should not match 101 (after 100, not specified)";
    EXPECT_FALSE(errors.isErrorCode(198)) << "Should not match 198 (before 199, not specified)";
    EXPECT_FALSE(errors.isErrorCode(201)) << "Should not match 201 (after 200, not specified)";
    EXPECT_FALSE(errors.isErrorCode(298)) << "Should not match 298 (before 299, not specified)";
    EXPECT_FALSE(errors.isErrorCode(300)) << "Should not match 300 (after 299)";
    EXPECT_FALSE(errors.isErrorCode(499)) << "Should not match 499 (just before 5xx range)";
    EXPECT_FALSE(errors.isErrorCode(600)) << "Should not match 600 (just after 5xx range)";
}

// Test with duplicate tokens (should still work correctly)
TEST_F(HttpTest, HttpStatusErrorsCustomArray5Test) {
    std::vector<std::string> tokens = {"404", "4xx", "404", "4xx"};
    HttpStatusErrors errors(tokens);
    
    // Should match 4xx range (includes 404)
    EXPECT_TRUE(errors.isErrorCode(400)) << "Should match 400 for 4xx category";
    EXPECT_TRUE(errors.isErrorCode(404)) << "Should match 404 (specific and in range)";
    EXPECT_TRUE(errors.isErrorCode(450)) << "Should match 450 for 4xx category";
    EXPECT_TRUE(errors.isErrorCode(499)) << "Should match 499 for 4xx category";
    
    // Should not match other ranges
    EXPECT_FALSE(errors.isErrorCode(300)) << "Should not match 300 (3xx not included)";
    EXPECT_FALSE(errors.isErrorCode(500)) << "Should not match 500 (5xx not included)";
    EXPECT_FALSE(errors.isErrorCode(200)) << "Should not match 200 (2xx not included)";
}

// Test complex realistic scenario
TEST_F(HttpTest, HttpStatusErrorsCustomArray6Test) {
    std::vector<std::string> tokens = {"4xx", "5xx", "200", "302"};
    HttpStatusErrors errors(tokens);
    
    // Should match client errors (4xx)
    EXPECT_TRUE(errors.isErrorCode(400)) << "Should match 400 for 4xx category";
    EXPECT_TRUE(errors.isErrorCode(401)) << "Should match 401 for 4xx category";
    EXPECT_TRUE(errors.isErrorCode(403)) << "Should match 403 for 4xx category";
    EXPECT_TRUE(errors.isErrorCode(404)) << "Should match 404 for 4xx category";
    EXPECT_TRUE(errors.isErrorCode(429)) << "Should match 429 for 4xx category";
    EXPECT_TRUE(errors.isErrorCode(499)) << "Should match 499 for 4xx category";
    
    // Should match server errors (5xx)
    EXPECT_TRUE(errors.isErrorCode(500)) << "Should match 500 for 5xx category";
    EXPECT_TRUE(errors.isErrorCode(502)) << "Should match 502 for 5xx category";
    EXPECT_TRUE(errors.isErrorCode(503)) << "Should match 503 for 5xx category";
    EXPECT_TRUE(errors.isErrorCode(504)) << "Should match 504 for 5xx category";
    EXPECT_TRUE(errors.isErrorCode(599)) << "Should match 599 for 5xx category";
    
    // Should match specific codes
    EXPECT_TRUE(errors.isErrorCode(200)) << "Should match specific code 200";
    EXPECT_TRUE(errors.isErrorCode(302)) << "Should match specific code 302";
    
    // Should not match other success codes
    EXPECT_FALSE(errors.isErrorCode(201)) << "Should not match 201 (other 2xx)";
    EXPECT_FALSE(errors.isErrorCode(204)) << "Should not match 204 (other 2xx)";
    
    // Should not match other redirect codes
    EXPECT_FALSE(errors.isErrorCode(301)) << "Should not match 301 (other 3xx)";
    EXPECT_FALSE(errors.isErrorCode(304)) << "Should not match 304 (other 3xx)";
    
    // Should not match informational codes
    EXPECT_FALSE(errors.isErrorCode(100)) << "Should not match 100 (1xx not included)";
    EXPECT_FALSE(errors.isErrorCode(101)) << "Should not match 101 (1xx not included)";
}

// Test with all categories
TEST_F(HttpTest, HttpStatusErrorsCustomArray7Test) {
    std::vector<std::string> tokens = {"1xx", "2xx", "3xx", "4xx", "5xx"};
    HttpStatusErrors errors(tokens);
    
    // Should match everything from 100-599
    EXPECT_TRUE(errors.isErrorCode(100)) << "Should match 100 for 1xx category";
    EXPECT_TRUE(errors.isErrorCode(150)) << "Should match 150 for 1xx category";
    EXPECT_TRUE(errors.isErrorCode(199)) << "Should match 199 for 1xx category";
    EXPECT_TRUE(errors.isErrorCode(200)) << "Should match 200 for 2xx category";
    EXPECT_TRUE(errors.isErrorCode(250)) << "Should match 250 for 2xx category";
    EXPECT_TRUE(errors.isErrorCode(299)) << "Should match 299 for 2xx category";
    EXPECT_TRUE(errors.isErrorCode(300)) << "Should match 300 for 3xx category";
    EXPECT_TRUE(errors.isErrorCode(350)) << "Should match 350 for 3xx category";
    EXPECT_TRUE(errors.isErrorCode(399)) << "Should match 399 for 3xx category";
    EXPECT_TRUE(errors.isErrorCode(400)) << "Should match 400 for 4xx category";
    EXPECT_TRUE(errors.isErrorCode(450)) << "Should match 450 for 4xx category";
    EXPECT_TRUE(errors.isErrorCode(499)) << "Should match 499 for 4xx category";
    EXPECT_TRUE(errors.isErrorCode(500)) << "Should match 500 for 5xx category";
    EXPECT_TRUE(errors.isErrorCode(550)) << "Should match 550 for 5xx category";
    EXPECT_TRUE(errors.isErrorCode(599)) << "Should match 599 for 5xx category";
    
    // Should not match out of range
    EXPECT_FALSE(errors.isErrorCode(99)) << "Should not match 99 (out of range)";
    EXPECT_FALSE(errors.isErrorCode(600)) << "Should not match 600 (out of range)";
    EXPECT_FALSE(errors.isErrorCode(700)) << "Should not match 700 (out of range)";
}

// Test unusual but valid specific codes
TEST_F(HttpTest, HttpStatusErrorsCustomArray8Test) {
    std::vector<std::string> tokens = {"102", "226", "308", "418", "451", "511"};
    HttpStatusErrors errors(tokens);
    
    // Should match only specified codes
    EXPECT_TRUE(errors.isErrorCode(102)) << "Should match specific code 102 (Processing)";
    EXPECT_TRUE(errors.isErrorCode(226)) << "Should match specific code 226 (IM Used)";
    EXPECT_TRUE(errors.isErrorCode(308)) << "Should match specific code 308 (Permanent Redirect)";
    EXPECT_TRUE(errors.isErrorCode(418)) << "Should match specific code 418 (I'm a teapot)";
    EXPECT_TRUE(errors.isErrorCode(451)) << "Should match specific code 451 (Unavailable For Legal Reasons)";
    EXPECT_TRUE(errors.isErrorCode(511)) << "Should match specific code 511 (Network Authentication Required)";
    
    // Should not match common codes in same ranges
    EXPECT_FALSE(errors.isErrorCode(100)) << "Should not match 100 (other 1xx)";
    EXPECT_FALSE(errors.isErrorCode(101)) << "Should not match 101 (other 1xx)";
    EXPECT_FALSE(errors.isErrorCode(200)) << "Should not match 200 (other 2xx)";
    EXPECT_FALSE(errors.isErrorCode(201)) << "Should not match 201 (other 2xx)";
    EXPECT_FALSE(errors.isErrorCode(300)) << "Should not match 300 (other 3xx)";
    EXPECT_FALSE(errors.isErrorCode(301)) << "Should not match 301 (other 3xx)";
    EXPECT_FALSE(errors.isErrorCode(400)) << "Should not match 400 (other 4xx)";
    EXPECT_FALSE(errors.isErrorCode(404)) << "Should not match 404 (other 4xx)";
    EXPECT_FALSE(errors.isErrorCode(500)) << "Should not match 500 (other 5xx)";
    EXPECT_FALSE(errors.isErrorCode(502)) << "Should not match 502 (other 5xx)";
}

// ========== HttpUrlFilter Class Tests ==========

// Test HttpUrlFilter with simple exact patterns
TEST_F(HttpTest, HttpUrlFilterSimplePatternsTest) {
    std::vector<std::string> cfg = {"/api/users", "/admin/dashboard", "/headers*"};
    HttpUrlFilter filter(cfg);
    
    // Should match exact URLs
    EXPECT_TRUE(filter.isFiltered("/api/users")) << "Should match exact URL /api/users";
    EXPECT_TRUE(filter.isFiltered("/admin/dashboard")) << "Should match exact URL /admin/dashboard";
    EXPECT_TRUE(filter.isFiltered("/headers")) << "Should match exact URL /headers";
    EXPECT_TRUE(filter.isFiltered("/headersall")) << "Should match exact URL /headersall";

    // Should not match different URLs
    EXPECT_FALSE(filter.isFiltered("/api/posts")) << "Should not match /api/posts";
    EXPECT_FALSE(filter.isFiltered("/headers/all")) << "Should not match /headers/all";
    EXPECT_FALSE(filter.isFiltered("/users")) << "Should not match /users";
    EXPECT_FALSE(filter.isFiltered("/admin")) << "Should not match /admin";
    EXPECT_FALSE(filter.isFiltered("/api/users/123")) << "Should not match longer path";
}

// Test HttpUrlFilter with single wildcard patterns
TEST_F(HttpTest, HttpUrlFilterSingleWildcardTest) {
    std::vector<std::string> cfg = {"/api/*", "/admin/*/settings"};
    HttpUrlFilter filter(cfg);
    
    // Should match single wildcard patterns
    EXPECT_TRUE(filter.isFiltered("/api/users")) << "Should match /api/* pattern";
    EXPECT_TRUE(filter.isFiltered("/api/posts")) << "Should match /api/* pattern";
    EXPECT_TRUE(filter.isFiltered("/api/test")) << "Should match /api/* pattern";
    
    EXPECT_TRUE(filter.isFiltered("/admin/user/settings")) << "Should match /admin/*/settings pattern";
    EXPECT_TRUE(filter.isFiltered("/admin/system/settings")) << "Should match /admin/*/settings pattern";
    EXPECT_TRUE(filter.isFiltered("/admin/config/settings")) << "Should match /admin/*/settings pattern";
    
    // Should not match paths that don't conform to pattern
    EXPECT_FALSE(filter.isFiltered("/api")) << "Should not match /api without following part";
    EXPECT_FALSE(filter.isFiltered("/api/users/123")) << "Should not match /api with multiple levels";
    EXPECT_FALSE(filter.isFiltered("/admin/user")) << "Should not match without /settings";
    EXPECT_FALSE(filter.isFiltered("/admin/user/config")) << "Should not match wrong ending";
    EXPECT_FALSE(filter.isFiltered("/other/path")) << "Should not match unrelated path";
}

// Test HttpUrlFilter with double wildcard patterns
TEST_F(HttpTest, HttpUrlFilterDoubleWildcardTest) {
    std::vector<std::string> cfg = {"/api/**", "/static/**"};
    HttpUrlFilter filter(cfg);
    
    // Should match any path under /api/ including multiple levels
    EXPECT_TRUE(filter.isFiltered("/api/users")) << "Should match single level under /api/";
    EXPECT_TRUE(filter.isFiltered("/api/users/123")) << "Should match multi-level under /api/";
    EXPECT_TRUE(filter.isFiltered("/api/v1/users/123/posts")) << "Should match deep nesting under /api/";
    EXPECT_TRUE(filter.isFiltered("/api/v2/admin/config/settings")) << "Should match complex path under /api/";
    
    EXPECT_TRUE(filter.isFiltered("/static/css")) << "Should match single level under /static/";
    EXPECT_TRUE(filter.isFiltered("/static/js/app.min.js")) << "Should match multi-level under /static/";
    EXPECT_TRUE(filter.isFiltered("/static/images/icons/favicon.ico")) << "Should match deep path under /static/";
    
    // Should not match without the prefix
    EXPECT_FALSE(filter.isFiltered("/users")) << "Should not match without /api prefix";
    EXPECT_FALSE(filter.isFiltered("/v1/users")) << "Should not match without /api prefix";
    EXPECT_FALSE(filter.isFiltered("/css/main.css")) << "Should not match without /static prefix";
}

// Test HttpUrlFilter with mixed wildcard patterns
TEST_F(HttpTest, HttpUrlFilterMixedWildcardTest) {
    std::vector<std::string> cfg = {"/api/*/users", "/files/*/download/**"};
    HttpUrlFilter filter(cfg);
    
    // Should match mixed patterns
    EXPECT_TRUE(filter.isFiltered("/api/v1/users")) << "Should match /api/*/users pattern";
    EXPECT_TRUE(filter.isFiltered("/api/v2/users")) << "Should match /api/*/users pattern";
    EXPECT_TRUE(filter.isFiltered("/api/admin/users")) << "Should match /api/*/users pattern";
    
    EXPECT_TRUE(filter.isFiltered("/files/public/download/doc.pdf")) << "Should match /files/*/download/** pattern";
    EXPECT_TRUE(filter.isFiltered("/files/private/download/images/photo.jpg")) << "Should match /files/*/download/** pattern";
    EXPECT_TRUE(filter.isFiltered("/files/temp/download/archive.zip")) << "Should match /files/*/download/** pattern";
    
    // Should not match incorrect patterns
    EXPECT_FALSE(filter.isFiltered("/api/users")) << "Should not match without middle part";
    EXPECT_FALSE(filter.isFiltered("/api/v1/posts")) << "Should not match with wrong ending";
    EXPECT_FALSE(filter.isFiltered("/files/public")) << "Should not match without /download part";
    EXPECT_FALSE(filter.isFiltered("/files/public/upload/file.txt")) << "Should not match with wrong middle part";
}

// Test HttpUrlFilter with special regex characters
TEST_F(HttpTest, HttpUrlFilterSpecialCharactersTest) {
    std::vector<std::string> cfg = {"/api/v1.0/users", "/path+with+plus", "/path[with]brackets", "/path{with}braces"};
    HttpUrlFilter filter(cfg);
    
    // Should match URLs with special characters literally
    EXPECT_TRUE(filter.isFiltered("/api/v1.0/users")) << "Should match URL with dot";
    EXPECT_TRUE(filter.isFiltered("/path+with+plus")) << "Should match URL with plus";
    EXPECT_TRUE(filter.isFiltered("/path[with]brackets")) << "Should match URL with brackets";
    EXPECT_TRUE(filter.isFiltered("/path{with}braces")) << "Should match URL with braces";
    
    // Should not treat special characters as regex (they should be escaped)
    EXPECT_FALSE(filter.isFiltered("/api/v1X0/users")) << "Dot should not match any character";
    EXPECT_FALSE(filter.isFiltered("/pathXwithXplus")) << "Plus should not be regex quantifier";
    EXPECT_FALSE(filter.isFiltered("/pathXwithXbrackets")) << "Brackets should not be character class";
    EXPECT_FALSE(filter.isFiltered("/pathXwithXbraces")) << "Braces should not be quantifier";
}

// Test HttpUrlFilter with complex realistic patterns
TEST_F(HttpTest, HttpUrlFilterRealisticPatternsTest) {
    std::vector<std::string> cfg = {
        "/api/v*/users/**",
        "/static/**",
        "/admin",
        "/health",
        "/metrics",
        "/docs/**"
    };
    HttpUrlFilter filter(cfg);
    
    // API patterns
    EXPECT_TRUE(filter.isFiltered("/api/v1/users/123")) << "Should match API user endpoint";
    EXPECT_TRUE(filter.isFiltered("/api/v2/users/456/profile")) << "Should match API user profile";
    EXPECT_TRUE(filter.isFiltered("/api/v10/users/admin/permissions")) << "Should match API admin permissions";
    
    // Static files
    EXPECT_TRUE(filter.isFiltered("/static/css/main.css")) << "Should match static CSS";
    EXPECT_TRUE(filter.isFiltered("/static/js/bundle.min.js")) << "Should match static JS";
    EXPECT_TRUE(filter.isFiltered("/static/images/logo.png")) << "Should match static images";
    
    // Exact endpoints
    EXPECT_TRUE(filter.isFiltered("/admin")) << "Should match admin endpoint";
    EXPECT_TRUE(filter.isFiltered("/health")) << "Should match health endpoint";
    EXPECT_TRUE(filter.isFiltered("/metrics")) << "Should match metrics endpoint";
    
    // Documentation
    EXPECT_TRUE(filter.isFiltered("/docs/api")) << "Should match docs";
    EXPECT_TRUE(filter.isFiltered("/docs/guides/installation")) << "Should match nested docs";
    
    // Should not match unrelated paths
    EXPECT_FALSE(filter.isFiltered("/api/users")) << "Should not match without version";
    EXPECT_FALSE(filter.isFiltered("/api/v1/posts")) << "Should not match non-user API";
    EXPECT_FALSE(filter.isFiltered("/admin/users")) << "Should not match admin subpaths";
    EXPECT_FALSE(filter.isFiltered("/public/file.txt")) << "Should not match non-static public files";
}

// Test HttpUrlFilter with empty configuration
TEST_F(HttpTest, HttpUrlFilterEmptyConfigTest) {
    std::vector<std::string> cfg = {};
    HttpUrlFilter filter(cfg);
    
    // Should not filter any URLs with empty config
    EXPECT_FALSE(filter.isFiltered("/api/users")) << "Should not filter with empty config";
    EXPECT_FALSE(filter.isFiltered("/")) << "Should not filter root with empty config";
    EXPECT_FALSE(filter.isFiltered("")) << "Should not filter empty string with empty config";
    EXPECT_FALSE(filter.isFiltered("/any/path/here")) << "Should not filter any path with empty config";
}

// Test HttpUrlFilter with root and subpath patterns
TEST_F(HttpTest, HttpUrlFilterRootAndSubpathTest) {
    std::vector<std::string> cfg = {"/*", "/api", "/api/*"};
    HttpUrlFilter filter(cfg);
    
    // Should match root level paths
    EXPECT_TRUE(filter.isFiltered("/home")) << "Should match root level path";
    EXPECT_TRUE(filter.isFiltered("/about")) << "Should match root level path";
    EXPECT_TRUE(filter.isFiltered("/contact")) << "Should match root level path";
    
    // Should match exact /api
    EXPECT_TRUE(filter.isFiltered("/api")) << "Should match exact /api";
    
    // Should match /api subpaths
    EXPECT_TRUE(filter.isFiltered("/api/users")) << "Should match /api subpath";
    EXPECT_TRUE(filter.isFiltered("/api/posts")) << "Should match /api subpath";
    
    // Note: /* pattern matches single level, so multi-level paths might not match
    // depending on the regex conversion logic
}

// Test HttpUrlFilter edge cases
TEST_F(HttpTest, HttpUrlFilterEdgeCasesTest) {
    std::vector<std::string> cfg = {"", "/", "//", "/**/", "/*/", "**"};
    HttpUrlFilter filter(cfg);
    
    // Test various edge case patterns
    EXPECT_TRUE(filter.isFiltered("")) << "Should handle empty pattern";
    EXPECT_TRUE(filter.isFiltered("/")) << "Should handle root pattern";
    
    // Note: The behavior for these edge cases depends on the regex conversion
    // Some might match, some might not, depending on implementation
    // These tests verify the filter doesn't crash on edge cases
}

// Test HttpUrlFilter with duplicate patterns
TEST_F(HttpTest, HttpUrlFilterDuplicatePatternsTest) {
    std::vector<std::string> cfg = {"/api/*", "/api/*", "/static/**", "/static/**"};
    HttpUrlFilter filter(cfg);
    
    // Should work correctly even with duplicate patterns
    EXPECT_TRUE(filter.isFiltered("/api/users")) << "Should work with duplicate patterns";
    EXPECT_TRUE(filter.isFiltered("/static/css/main.css")) << "Should work with duplicate patterns";
    
    EXPECT_FALSE(filter.isFiltered("/admin/panel")) << "Should not match unrelated paths";
}

// ========== HttpHeaderRecorder Class Tests ==========

// Test HttpHeaderRecorder with empty configuration
TEST_F(HttpTest, HttpHeaderRecorderEmptyConfigTest) {
    std::vector<std::string> cfg = {};
    HttpHeaderRecorder recorder(100, cfg);
    
    // Create test headers
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer token123";
    MockHeaderReader headerReader(headers);
    
    auto annotation = std::make_shared<MockAnnotation>();
    recorder.recordHeader(headerReader, annotation);
    
    // Should not record any headers with empty config
    EXPECT_EQ(annotation->GetStringStringCount(), 0) << "Should not record headers with empty config";
}

// Test HttpHeaderRecorder with specific header configuration
TEST_F(HttpTest, HttpHeaderRecorderSpecificHeadersTest) {
    std::vector<std::string> cfg = {"Content-Type", "Authorization"};
    HttpHeaderRecorder recorder(200, cfg);
    
    // Create test headers with more than configured
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer token123";
    headers["User-Agent"] = "TestAgent/1.0";
    headers["Accept"] = "application/json";
    MockHeaderReader headerReader(headers);
    
    auto annotation = std::make_shared<MockAnnotation>();
    recorder.recordHeader(headerReader, annotation);
    
    // Should record only configured headers
    EXPECT_EQ(annotation->GetStringStringCount(), 2) << "Should record exactly 2 headers";
    
    // Check that annotations were recorded with the correct key
    const auto& stringStringValues = annotation->GetStringStringValues();
    auto it = stringStringValues.find(200);
    EXPECT_NE(it, stringStringValues.end()) << "Should have annotation with key 200";
    EXPECT_EQ(it->second.size(), 2) << "Should have 2 headers recorded with key 200";
    
    // Verify the recorded headers contain our expected values
    // Note: The implementation records each header separately with the same key
    // We need to check if the recorded values are correct
}

// Test HttpHeaderRecorder with single header configuration
TEST_F(HttpTest, HttpHeaderRecorderSingleHeaderTest) {
    std::vector<std::string> cfg = {"Content-Type"};
    HttpHeaderRecorder recorder(300, cfg);
    
    // Create test headers
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "text/html";
    headers["Cache-Control"] = "no-cache";
    MockHeaderReader headerReader(headers);
    
    auto annotation = std::make_shared<MockAnnotation>();
    recorder.recordHeader(headerReader, annotation);
    
    // Should record only Content-Type header
    EXPECT_EQ(annotation->GetStringStringCount(), 1) << "Should record exactly 1 header";
    
    const auto& stringStringValues = annotation->GetStringStringValues();
    auto it = stringStringValues.find(300);
    EXPECT_NE(it, stringStringValues.end()) << "Should have annotation with key 300";
    EXPECT_EQ(it->second.size(), 1) << "Should have 1 header recorded with key 300";
}

// Test HttpHeaderRecorder with missing headers
TEST_F(HttpTest, HttpHeaderRecorderMissingHeadersTest) {
    std::vector<std::string> cfg = {"Content-Type", "Authorization", "X-Custom-Header"};
    HttpHeaderRecorder recorder(400, cfg);
    
    // Create test headers with only some of the configured headers
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/xml";
    // Missing Authorization and X-Custom-Header
    MockHeaderReader headerReader(headers);
    
    auto annotation = std::make_shared<MockAnnotation>();
    recorder.recordHeader(headerReader, annotation);
    
    // Should record only the available header (Content-Type)
    EXPECT_EQ(annotation->GetStringStringCount(), 1) << "Should record exactly 1 header (only available one)";
    
    const auto& stringStringValues = annotation->GetStringStringValues();
    auto it = stringStringValues.find(400);
    EXPECT_NE(it, stringStringValues.end()) << "Should have annotation with key 400";
    EXPECT_EQ(it->second.size(), 1) << "Should have 1 header recorded with key 400";
}

// Test HttpHeaderRecorder with HEADERS-ALL configuration
TEST_F(HttpTest, HttpHeaderRecorderAllHeadersTest) {
    std::vector<std::string> cfg = {"HEADERS-ALL"};
    HttpHeaderRecorder recorder(500, cfg);
    
    // Create test headers
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer token123";
    headers["User-Agent"] = "TestAgent/1.0";
    headers["Accept"] = "application/json";
    headers["Cache-Control"] = "no-cache";
    MockHeaderReader headerReader(headers);
    
    auto annotation = std::make_shared<MockAnnotation>();
    recorder.recordHeader(headerReader, annotation);
    
    // Should record all headers
    EXPECT_EQ(annotation->GetStringStringCount(), 5) << "Should record all 5 headers";
    
    // All should have the same annotation key (500)
    const auto& stringStringValues = annotation->GetStringStringValues();
    auto it = stringStringValues.find(500);
    EXPECT_NE(it, stringStringValues.end()) << "Should have annotations with key 500";
    EXPECT_EQ(it->second.size(), 5) << "Should have 5 headers recorded with key 500";
}

// Test HttpHeaderRecorder with case sensitivity
TEST_F(HttpTest, HttpHeaderRecorderCaseSensitivityTest) {
    std::vector<std::string> cfg = {"content-type", "Content-Type"};
    HttpHeaderRecorder recorder(600, cfg);
    
    // Create test headers
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["content-type"] = "text/plain"; // Different case
    MockHeaderReader headerReader(headers);
    
    auto annotation = std::make_shared<MockAnnotation>();
    recorder.recordHeader(headerReader, annotation);
    
    // The behavior depends on the HeaderReader implementation
    // Our mock treats keys as case-sensitive
    EXPECT_GE(annotation->GetStringStringCount(), 1) << "Should record at least 1 header";
    EXPECT_LE(annotation->GetStringStringCount(), 2) << "Should record at most 2 headers";
}

// Test HttpHeaderRecorder with empty header values
TEST_F(HttpTest, HttpHeaderRecorderEmptyValuesTest) {
    std::vector<std::string> cfg = {"X-Empty-Header", "X-Normal-Header"};
    HttpHeaderRecorder recorder(700, cfg);
    
    // Create test headers with empty value
    std::map<std::string, std::string> headers;
    headers["X-Empty-Header"] = "";
    headers["X-Normal-Header"] = "normal-value";
    MockHeaderReader headerReader(headers);
    
    auto annotation = std::make_shared<MockAnnotation>();
    recorder.recordHeader(headerReader, annotation);
    
    // Should record both headers, including the one with empty value
    EXPECT_EQ(annotation->GetStringStringCount(), 2) << "Should record both headers including empty value";
    
    const auto& stringStringValues = annotation->GetStringStringValues();
    auto it = stringStringValues.find(700);
    EXPECT_NE(it, stringStringValues.end()) << "Should have annotations with key 700";
    EXPECT_EQ(it->second.size(), 2) << "Should have 2 headers recorded with key 700";
}

// Test HttpHeaderRecorder with special characters in headers
TEST_F(HttpTest, HttpHeaderRecorderSpecialCharactersTest) {
    std::vector<std::string> cfg = {"X-Special-Chars"};
    HttpHeaderRecorder recorder(800, cfg);
    
    // Create test headers with special characters
    std::map<std::string, std::string> headers;
    headers["X-Special-Chars"] = "value with spaces, commas; and: colons=equals";
    MockHeaderReader headerReader(headers);
    
    auto annotation = std::make_shared<MockAnnotation>();
    recorder.recordHeader(headerReader, annotation);
    
    // Should record header with special characters
    EXPECT_EQ(annotation->GetStringStringCount(), 1) << "Should record header with special characters";
    
    const auto& stringStringValues = annotation->GetStringStringValues();
    auto it = stringStringValues.find(800);
    EXPECT_NE(it, stringStringValues.end()) << "Should have annotation with key 800";
    EXPECT_EQ(it->second.size(), 1) << "Should have 1 header recorded with key 800";
}

// Test HttpHeaderRecorder with multiple configurations including HEADERS-ALL (edge case)
TEST_F(HttpTest, HttpHeaderRecorderMultipleAllConfigTest) {
    std::vector<std::string> cfg = {"HEADERS-ALL", "Content-Type"}; // Mixed config
    HttpHeaderRecorder recorder(900, cfg);
    
    // Create test headers
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer token123";
    MockHeaderReader headerReader(headers);
    
    auto annotation = std::make_shared<MockAnnotation>();
    recorder.recordHeader(headerReader, annotation);
    
    // Should NOT behave as HEADERS-ALL since cfg size > 1
    // Only "Content-Type" should be recorded ("HEADERS-ALL" is not a real header name)
    EXPECT_EQ(annotation->GetStringStringCount(), 1) << "Should record only Content-Type header (HEADERS-ALL requires single element config)";
    
    const auto& stringStringValues = annotation->GetStringStringValues();
    auto it = stringStringValues.find(900);
    EXPECT_NE(it, stringStringValues.end()) << "Should have annotations with key 900";
    EXPECT_EQ(it->second.size(), 1) << "Should have 1 header recorded with key 900";
}

// Test HttpHeaderRecorder with realistic HTTP headers
TEST_F(HttpTest, HttpHeaderRecorderRealisticHeadersTest) {
    std::vector<std::string> cfg = {
        "Content-Type", 
        "Authorization", 
        "User-Agent", 
        "Accept",
        "X-Forwarded-For",
        "X-Request-ID"
    };
    HttpHeaderRecorder recorder(1000, cfg);
    
    // Create realistic HTTP headers
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json; charset=utf-8";
    headers["Authorization"] = "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...";
    headers["User-Agent"] = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36";
    headers["Accept"] = "application/json, text/plain, */*";
    headers["X-Forwarded-For"] = "203.0.113.195, 198.51.100.178";
    headers["X-Request-ID"] = "req-123e4567-e89b-12d3-a456-426614174000";
    headers["Cookie"] = "session=abc123; preferences=dark_mode"; // Not in config
    MockHeaderReader headerReader(headers);
    
    auto annotation = std::make_shared<MockAnnotation>();
    recorder.recordHeader(headerReader, annotation);
    
    // Should record only configured headers (6 out of 7)
    EXPECT_EQ(annotation->GetStringStringCount(), 6) << "Should record exactly 6 configured headers";
    
    const auto& stringStringValues = annotation->GetStringStringValues();
    auto it = stringStringValues.find(1000);
    EXPECT_NE(it, stringStringValues.end()) << "Should have annotations with key 1000";
    EXPECT_EQ(it->second.size(), 6) << "Should have 6 headers recorded with key 1000";
}

// Test HttpHeaderRecorder with no headers available
TEST_F(HttpTest, HttpHeaderRecorderNoHeadersTest) {
    std::vector<std::string> cfg = {"Content-Type", "Authorization"};
    HttpHeaderRecorder recorder(1100, cfg);
    
    // Create empty headers
    std::map<std::string, std::string> headers; // Empty
    MockHeaderReader headerReader(headers);
    
    auto annotation = std::make_shared<MockAnnotation>();
    recorder.recordHeader(headerReader, annotation);
    
    // Should not record any headers
    EXPECT_EQ(annotation->GetStringStringCount(), 0) << "Should not record any headers when none are available";
}

// ========== HttpMethodFilter Class Tests ==========

// Test HttpMethodFilter with single method
TEST_F(HttpTest, HttpMethodFilterSingleMethodTest) {
    std::vector<std::string> cfg = {"POST"};
    HttpMethodFilter filter(cfg);
    
    // Should filter POST method
    EXPECT_TRUE(filter.isFiltered("POST")) << "Should filter POST method";
    
    // Should not filter other methods
    EXPECT_FALSE(filter.isFiltered("GET")) << "Should not filter GET method";
    EXPECT_FALSE(filter.isFiltered("PUT")) << "Should not filter PUT method";
    EXPECT_FALSE(filter.isFiltered("DELETE")) << "Should not filter DELETE method";
}

// Test HttpMethodFilter with multiple methods
TEST_F(HttpTest, HttpMethodFilterMultipleMethodsTest) {
    std::vector<std::string> cfg = {"POST", "PUT", "DELETE"};
    HttpMethodFilter filter(cfg);
    
    // Should filter specified methods
    EXPECT_TRUE(filter.isFiltered("POST")) << "Should filter POST method";
    EXPECT_TRUE(filter.isFiltered("PUT")) << "Should filter PUT method";
    EXPECT_TRUE(filter.isFiltered("DELETE")) << "Should filter DELETE method";
    
    // Should not filter unspecified methods
    EXPECT_FALSE(filter.isFiltered("GET")) << "Should not filter GET method";
    EXPECT_FALSE(filter.isFiltered("HEAD")) << "Should not filter HEAD method";
}

// Test HttpMethodFilter with case sensitivity
TEST_F(HttpTest, HttpMethodFilterCaseInsensitiveTest) {
    std::vector<std::string> cfg = {"POST"};
    HttpMethodFilter filter(cfg);
    
    // Should handle different cases (assuming compare_string is case-insensitive)
    EXPECT_TRUE(filter.isFiltered("POST")) << "Should filter POST";
    EXPECT_TRUE(filter.isFiltered("post")) << "Should filter post (case insensitive)";
    EXPECT_TRUE(filter.isFiltered("Post")) << "Should filter Post (case insensitive)";
}

// Test HttpMethodFilter with empty configuration
TEST_F(HttpTest, HttpMethodFilterEmptyConfigTest) {
    std::vector<std::string> cfg = {};
    HttpMethodFilter filter(cfg);
    
    // Should not filter any methods
    EXPECT_FALSE(filter.isFiltered("GET")) << "Should not filter with empty config";
    EXPECT_FALSE(filter.isFiltered("POST")) << "Should not filter with empty config";
    EXPECT_FALSE(filter.isFiltered("")) << "Should not filter with empty config";
}

} // namespace pinpoint
