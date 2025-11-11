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

    /**
     * @brief Strategy interface for testing whether an HTTP status code indicates an error.
     */
    class HttpStatusCode {
    public:
        virtual ~HttpStatusCode() = default;
        /**
         * @brief Returns `true` when the provided status code should be considered an error.
         */
        virtual bool isError(int status_code) noexcept = 0;
    };

    /**
     * @brief Matches a single status code for error detection.
     */
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

    /**
     * @brief Matches informational (100-199) status codes.
     */
    class HttpStatusInformational : public HttpStatusCode {
    public:
        bool isError(int status_code) noexcept override {
            return 100 <= status_code && status_code <= 199;
        }
    };

    /**
     * @brief Matches success (200-299) status codes.
     */
    class HttpStatusSuccess : public HttpStatusCode {
    public:
        bool isError(int status_code) noexcept override {
            return 200 <= status_code && status_code <= 299;
        }
    };

    /**
     * @brief Matches redirection (300-399) status codes.
     */
    class HttpStatusRedirection : public HttpStatusCode {
    public:
        bool isError(int status_code) noexcept override {
            return 300 <= status_code && status_code <= 399;
        }
    };

    /**
     * @brief Matches client error (400-499) status codes.
     */
    class HttpStatusClientError : public HttpStatusCode {
    public:
        bool isError(int status_code) noexcept override {
            return 400 <= status_code && status_code <= 499;
        }
    };

    /**
     * @brief Matches server error (500-599) status codes.
     */
    class HttpStatusServerError : public HttpStatusCode {
    public:
        bool isError(int status_code) noexcept override {
            return 500 <= status_code && status_code <= 599;
        }
    };

    /**
     * @brief Parses status expressions from the configuration and evaluates failure conditions.
     */
    class HttpStatusErrors {
    public:
        explicit HttpStatusErrors(const std::vector<std::string>& tokens);
        ~HttpStatusErrors() = default;

        /**
         * @brief Returns whether an HTTP status code should be treated as failure.
         */
        bool isErrorCode(int status_code) const noexcept;

    private:
        std::vector<HttpStatusCode *> errors;
    };

    /**
     * @brief Captures selected HTTP headers and appends them to span annotations.
     */
    class HttpHeaderRecorder {
    public:
        HttpHeaderRecorder(int anno_key, std::vector<std::string>& cfg);
        ~HttpHeaderRecorder() = default;

        /**
         * @brief Records headers using the configuration rules.
         *
         * @param header Header reader callback provided by the user.
         * @param annotation Annotation destination that receives captured key-value pairs.
         */
        void recordHeader(const HeaderReader& header, AnnotationPtr annotation);
    private:
        int anno_key_;
        std::vector<std::string> cfg_;
        bool dump_all_headers_ = false;
    };

    /**
     * @brief Filters URLs based on Ant-style patterns supplied via configuration.
     */
    class HttpUrlFilter {
    public:
        HttpUrlFilter(const std::vector<std::string>& cfg);
        ~HttpUrlFilter() = default;

        /**
         * @brief Tests whether a URL should be ignored for statistics.
         *
         * @param url URL to test.
         * @return `true` if the pattern list matches the URL.
         */
        bool isFiltered(std::string_view url) const;

    private:
        std::vector<std::string> cfg_;
        std::vector<std::regex> pattern_;

        /// @brief Converts an Ant-style path to a regular expression.
        static std::string convert_to_regex(std::string_view antPath);
        /// @brief Appends a single character to the regex buffer, escaping when needed.
        static void write_char(std::stringstream& buf, char c);
    };

    /**
     * @brief Filters HTTP methods according to a configuration whitelist or blacklist.
     */
    class HttpMethodFilter {
    public:
        HttpMethodFilter(const std::vector<std::string>& cfg);
        ~HttpMethodFilter() = default;

        /**
         * @brief Tests whether a method should be ignored.
         *
         * @param method Method string to test (e.g., GET, POST).
         * @return `true` if filtering rules match the method.
         */
        bool isFiltered(std::string_view method) const;
    private:
        std::vector<std::string> cfg_;
    };

}
