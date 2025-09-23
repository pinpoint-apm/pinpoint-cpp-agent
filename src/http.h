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

#pragma once

#include <regex>
#include <vector>

#include "pinpoint/tracer.h"

namespace pinpoint {

    class HttpStatusCode {
    public:
        virtual ~HttpStatusCode() = default;
        virtual bool isError(int status_code) noexcept = 0;
    };

    class HttpStatusDefault: public HttpStatusCode {
    public:
        explicit HttpStatusDefault(int code) {
            status_code_ = code;
        }

        bool isError(int status_code) noexcept override {
            return status_code_ == status_code;
        }

    private:
        int status_code_;
    };

    class HttpStatusInformational : public HttpStatusCode {
    public:
        bool isError(int status_code) noexcept override {
            return 100 <= status_code && status_code <= 199;
        }
    };

    class HttpStatusSuccess : public HttpStatusCode {
    public:
        bool isError(int status_code) noexcept override {
            return 200 <= status_code && status_code <= 299;
        }
    };

    class HttpStatusRedirection : public HttpStatusCode {
    public:
        bool isError(int status_code) noexcept override {
            return 300 <= status_code && status_code <= 399;
        }
    };

    class HttpStatusClientError : public HttpStatusCode {
    public:
        bool isError(int status_code) noexcept override {
            return 400 <= status_code && status_code <= 499;
        }
    };

    class HttpStatusServerError : public HttpStatusCode {
    public:
        bool isError(int status_code) noexcept override {
            return 500 <= status_code && status_code <= 599;
        }
    };

    class HttpStatusErrors {
    public:
        explicit HttpStatusErrors(const std::vector<std::string>& tokens);
        ~HttpStatusErrors() = default;

        bool isErrorCode(int status_code) const noexcept;

    private:
        std::vector<HttpStatusCode *> errors;
    };

    class HttpHeaderRecorder {
    public:
        HttpHeaderRecorder(int anno_key, std::vector<std::string>& cfg);
        ~HttpHeaderRecorder() = default;

        void recordHeader(const HeaderReader& header, AnnotationPtr annotation);
    private:
        int anno_key_;
        std::vector<std::string> cfg_;
        bool dump_all_headers_ = false;
    };

    class HttpUrlFilter {
    public:
        HttpUrlFilter(const std::vector<std::string>& cfg);
        ~HttpUrlFilter() = default;

        bool isFiltered(std::string_view url) const;

    private:
        std::vector<std::string> cfg_;
        std::vector<std::regex> pattern_;

        static std::string convert_to_regex(std::string_view antPath);
        static void write_char(std::stringstream& buf, char c);
    };

    class HttpMethodFilter {
    public:
        HttpMethodFilter(const std::vector<std::string>& cfg);
        ~HttpMethodFilter() = default;

        bool isFiltered(std::string_view method) const;
    private:
        std::vector<std::string> cfg_;
    };

}
