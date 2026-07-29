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

#include <bitset>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pinpoint/tracer.h"

namespace pinpoint {

    // HTTP status code range constants
    namespace http_status {
        constexpr int INFORMATIONAL_MIN = 100;
        constexpr int INFORMATIONAL_MAX = 199;
        constexpr int SUCCESS_MIN = 200;
        constexpr int SUCCESS_MAX = 299;
        constexpr int REDIRECTION_MIN = 300;
        constexpr int REDIRECTION_MAX = 399;
        constexpr int CLIENT_ERROR_MIN = 400;
        constexpr int CLIENT_ERROR_MAX = 499;
        constexpr int SERVER_ERROR_MIN = 500;
        constexpr int SERVER_ERROR_MAX = 599;
        // Size of the direct-lookup error table: covers every standard code.
        constexpr int TABLE_SIZE = SERVER_ERROR_MAX + 1;
    }

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
        explicit HttpStatusDefault(int code) : status_code_(code) {}

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
            return http_status::INFORMATIONAL_MIN <= status_code && 
                   status_code <= http_status::INFORMATIONAL_MAX;
        }
    };

    /**
     * @brief Matches success (200-299) status codes.
     */
    class HttpStatusSuccess : public HttpStatusCode {
    public:
        bool isError(int status_code) noexcept override {
            return http_status::SUCCESS_MIN <= status_code && 
                   status_code <= http_status::SUCCESS_MAX;
        }
    };

    /**
     * @brief Matches redirection (300-399) status codes.
     */
    class HttpStatusRedirection : public HttpStatusCode {
    public:
        bool isError(int status_code) noexcept override {
            return http_status::REDIRECTION_MIN <= status_code && 
                   status_code <= http_status::REDIRECTION_MAX;
        }
    };

    /**
     * @brief Matches client error (400-499) status codes.
     */
    class HttpStatusClientError : public HttpStatusCode {
    public:
        bool isError(int status_code) noexcept override {
            return http_status::CLIENT_ERROR_MIN <= status_code && 
                   status_code <= http_status::CLIENT_ERROR_MAX;
        }
    };

    /**
     * @brief Matches server error (500-599) status codes.
     */
    class HttpStatusServerError : public HttpStatusCode {
    public:
        bool isError(int status_code) noexcept override {
            return http_status::SERVER_ERROR_MIN <= status_code && 
                   status_code <= http_status::SERVER_ERROR_MAX;
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
        // Configured tokens in [0, 599] are flattened into a bit table at
        // construction, so the common per-span check is a bounds check plus a
        // constant-time bit lookup. Explicit codes outside that range use the
        // small fallback vector.
        std::bitset<http_status::TABLE_SIZE> error_codes_{};
        std::vector<int> extra_codes_{};  // configured codes outside [0, TABLE_SIZE)
    };

    /**
     * @brief Captures selected HTTP headers and appends them to span annotations.
     */
    class HttpHeaderRecorder {
    public:
        HttpHeaderRecorder(int anno_key, std::vector<std::string> cfg);
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
     *
     * Uses direct string matching instead of std::regex for performance.
     * Supports `*` (matches within a single path segment), `**` (matches across
     * segments), and `?` (matches exactly one character within a segment).
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
        enum class PatternKind {
            Exact,
            Prefix,
            SegmentPrefix,
            Ant,
        };

        enum class TokenKind {
            Literal,
            Star,
            DoubleStar,
            DoubleStarSlash,
            Question,
        };

        struct PatternToken {
            TokenKind kind;
            char value = '\0';
        };

        struct CompiledPattern {
            PatternKind kind = PatternKind::Ant;
            std::string pattern;
            std::string literal_prefix;
            size_t min_length = 0;
            std::vector<PatternToken> tokens;
        };

        struct MatchScratch {
            std::vector<char> current;
            std::vector<char> next;
        };

        // Per-thread scratch storage, split into a trivially-destructible
        // slot plus a separate reclaim guard so a match during thread
        // teardown stays defined behavior — the same shape as
        // AtomicSharedPtr::thread_cache(). A block-scope thread_local with a
        // destructor must not be passed through again once destroyed
        // ([basic.start.term]) — yet a host thread_local destructor that
        // records a final span during thread exit reaches isFiltered()
        // exactly then (AgentImpl::NewSpan runs the URL filter), and TLS
        // destruction order would have already destroyed a plain
        // thread_local MatchScratch constructed after the host's object. The
        // slot has no destructor, so it is never "destroyed" and stays valid
        // for the whole thread lifetime; only the scratch it points at is
        // reclaimed, by the guard.
        struct MatchScratchSlot {
            MatchScratch* scratch = nullptr;
            // Set by the guard's destructor: from then on match_scratch()
            // takes the leak path instead of re-registering a guard
            // (impossible once TLS destructors have started).
            bool reclaimed = false;
        };
        struct MatchScratchReclaim {
            ~MatchScratchReclaim();
        };
        static MatchScratchSlot& match_scratch_slot();
        static MatchScratch& match_scratch();

        std::vector<CompiledPattern> patterns_;

        static CompiledPattern compilePattern(const std::string& pattern);
        static bool ant_match(const CompiledPattern& pattern, std::string_view url, MatchScratch& scratch);
    };

    /**
     * @brief Filters HTTP methods according to a configuration whitelist or blacklist.
     */
    class HttpMethodFilter {
    public:
        HttpMethodFilter(std::vector<std::string> cfg);
        ~HttpMethodFilter() = default;

        /**
         * @brief Tests whether a method should be ignored.
         *
         * @param method Method string to test (e.g., GET, POST).
         * @return `true` if filtering rules match the method.
         */
        bool isFiltered(std::string_view method) const;
    private:
        std::vector<std::string> methods_;
    };

    /**
     * @brief Utility functions for HTTP tracing.
     */
    class HttpTracerUtil {
    public:
        /**
         * @brief Extracts the real remote address from HTTP headers.
         * 
         * Checks X-Forwarded-For and X-Real-Ip headers for proxy information,
         * falling back to the direct remote address if not present.
         *
         * @param reader Header reader to access HTTP headers.
         * @param remote_addr Direct remote address (e.g., from socket).
         * @return The actual remote client address.
         */
        static std::string getRemoteAddr(const HeaderReader& reader, std::string_view remote_addr);

        /**
         * @brief Records proxy header information to span annotations.
         * 
         * Parses Pinpoint proxy headers (Apache, Nginx, or App) and adds
         * timing and load information to the span.
         *
         * @param reader Header reader to access HTTP headers.
         * @param annotation Annotation destination for proxy metadata.
         */
        static void setProxyHeader(const HeaderReader& reader, AnnotationPtr annotation);
    };

}
