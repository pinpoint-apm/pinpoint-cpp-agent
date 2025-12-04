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

#include <algorithm>
#include <cstring>
#include <regex>
#include <string>

#include "logging.h"
#include "utility.h"
#include "http.h"

namespace pinpoint {

    HttpStatusErrors::HttpStatusErrors(const std::vector<std::string>& tokens) {
        for (const auto& token : tokens){
            if (compare_string(token, "5xx")) {
                errors.push_back(std::make_unique<HttpStatusServerError>());
            } else if (compare_string(token, "4xx")) {
                errors.push_back(std::make_unique<HttpStatusClientError>());
            } else if (compare_string(token, "3xx")) {
                errors.push_back(std::make_unique<HttpStatusRedirection>());
            } else if (compare_string(token, "2xx")) {
                errors.push_back(std::make_unique<HttpStatusSuccess>());
            } else if (compare_string(token, "1xx")) {
                errors.push_back(std::make_unique<HttpStatusInformational>());
            } else {
                auto result = stoi_(token);
                if (result.has_value()) {
                    errors.push_back(std::make_unique<HttpStatusDefault>(result.value()));
                }
                // If parsing fails, ignore the invalid token
            }
        }
    }

    bool HttpStatusErrors::isErrorCode(int status_code) const noexcept {
        return std::any_of(errors.begin(), errors.end(),
            [status_code](const auto& code) {
                return code->isError(status_code);
            });
    }

    HttpHeaderRecorder::HttpHeaderRecorder(int anno_key, std::vector<std::string> cfg) 
        : anno_key_(anno_key), 
          cfg_(std::move(cfg)),
          dump_all_headers_(cfg_.size() == 1 && compare_string(cfg_[0], "HEADERS-ALL")) {
    }

    void HttpHeaderRecorder::recordHeader(const HeaderReader& header, AnnotationPtr annotation) {
        if (cfg_.empty()) {
            return;
        }

        if (dump_all_headers_) {
            header.ForEach([this, annotation](std::string_view key, std::string_view val) {
                annotation->AppendStringString(anno_key_, key, val);
                return true;
            });
        } else {
            for (const auto& header_name : cfg_) {
                if (const auto v = header.Get(header_name); v.has_value()) {
                    annotation->AppendStringString(anno_key_, header_name, v.value());
                }
            }
        }
    }

    HttpUrlFilter::HttpUrlFilter(const std::vector<std::string>& cfg) {
        pattern_.reserve(cfg.size());
        for (const auto& pattern_str : cfg) {
            try {
                pattern_.emplace_back(convert_to_regex(pattern_str));
            } catch (const std::regex_error& e) {
                LOG_WARN("Invalid URL pattern '{}': {}", pattern_str, e.what());
                // Continue processing other patterns
            }
        }
    }

    bool HttpUrlFilter::isFiltered(std::string_view url) const {
        const std::string url_str(url);  // Convert to null-terminated string for regex_match
        for (const auto& pattern : pattern_) {
            if (std::regex_match(url_str, pattern)) {
                return true;
            }
        }
        return false;
    }

    std::string HttpUrlFilter::convert_to_regex(std::string_view antPath) {
        std::string result;
        result.reserve(antPath.size() + 10);  // Pre-allocate to avoid reallocation
        result += '^';

        bool after_start = false;
        for (char c : antPath) {
            if (after_start) {
                if (c == '*') {
                    result += ".*";
                } else {
                    result += "[^/]*";
                    append_escaped_char(result, c);
                }
                after_start = false;
            } else {
                if (c == '*') {
                    after_start = true;
                } else {
                    append_escaped_char(result, c);
                }
            }
        }
        if (after_start) {
            result += "[^/]*";
        }

        result += '$';
        return result;
    }

    void HttpUrlFilter::append_escaped_char(std::string& buf, char c) {
        // Regex special characters that need escaping
        constexpr char special_chars[] = ".+^$[]{}()|?\\*";
        
        if (std::strchr(special_chars, c) != nullptr) {
            buf += '\\';
        }
        buf += c;
    }

    HttpMethodFilter::HttpMethodFilter(const std::vector<std::string>& cfg) : cfg_(cfg) {}

    bool HttpMethodFilter::isFiltered(std::string_view method) const {
        for (const auto& method_name : cfg_) {
            if (compare_string(method, method_name)) {
                return true;
            }
        }
        return false;
    }

}
