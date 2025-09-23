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

#include <iostream>
#include <regex>
#include <sstream>
#include <string>

#include "utility.h"
#include "http.h"

namespace pinpoint {

    HttpStatusErrors::HttpStatusErrors(const std::vector<std::string>& tokens) {
        for (const std::string& token : tokens){
            if (compare_string(token, "5xx")) {
                errors.push_back(new HttpStatusServerError());
            } else if (compare_string(token, "4xx")) {
                errors.push_back(new HttpStatusClientError());
            } else if (compare_string(token, "3xx")) {
                errors.push_back(new HttpStatusRedirection());
            } else if (compare_string(token, "2xx")) {
                errors.push_back(new HttpStatusSuccess());
            } else if (compare_string(token, "1xx")) {
                errors.push_back(new HttpStatusInformational());
            } else {
                auto result = stoi_(token);
                if (result.has_value()) {
                    errors.push_back(new HttpStatusDefault(result.value()));
                }
                // If parsing fails, ignore the invalid token
            }
        }
    }

    bool HttpStatusErrors::isErrorCode(int status_code) const noexcept {
        for (HttpStatusCode* code: errors){
            if (code->isError(status_code)) {
                return true;
            }
        }
        return false;
    }

    HttpHeaderRecorder::HttpHeaderRecorder(int anno_key, std::vector<std::string>& cfg) : anno_key_(anno_key), cfg_(cfg) {
        if (cfg_.size() == 1 && compare_string(cfg_[0], "HEADERS-ALL")) {
            dump_all_headers_ = true;
        }
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
            for (auto iter = cfg_.begin(); iter != cfg_.end(); ++iter) {
                if (const auto v = header.Get(*iter); v.has_value()) {
                    annotation->AppendStringString(anno_key_, *iter, v.value());
                }
            }
        }
    }

    HttpUrlFilter::HttpUrlFilter(const std::vector<std::string>& cfg) : cfg_(cfg) {
        for (auto& iter : cfg_) {
            auto p = convert_to_regex(iter);
            pattern_.emplace_back(p);
        }
    }

    bool HttpUrlFilter::isFiltered(std::string_view url) const {
        for (auto& iter : pattern_) {
            if (std::regex_match(url.data(), iter)) {
                return true;
            }
        }
        return false;
    }

    std::string HttpUrlFilter::convert_to_regex(std::string_view antPath) {
        std::stringstream buf;
        buf << '^';

        bool after_start = false;
        for (char c : antPath) {
            if (after_start) {
                if (c == '*') {
                    buf << ".*";
                } else {
                    buf << "[^/]*";
                    write_char(buf, c);
                }
                after_start = false;
            } else {
                if (c == '*') {
                    after_start = true;
                } else {
                    write_char(buf, c);
                }
            }
        }
        if (after_start) {
            buf << "[^/]*";
        }

        buf << '$';
        return buf.str();
    }

    void HttpUrlFilter::write_char(std::stringstream& buf, char c) {
        if (c == '.' || c == '+' || c == '^' || c == '[' || c == ']' || c == '{' || c == '}') {
            buf << '\\';
        }
        buf << c;
    }

    HttpMethodFilter::HttpMethodFilter(const std::vector<std::string>& cfg) : cfg_(cfg) {}

    bool HttpMethodFilter::isFiltered(std::string_view method) const {
        for (auto& iter : cfg_) {
            if (compare_string(method, iter)) {
                return true;
            }
        }
        return false;
    }

}
