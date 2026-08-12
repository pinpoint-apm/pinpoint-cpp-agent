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

    class PinpointAnnotation;

    namespace http_status {
        // Size of the direct-lookup error table: covers every standard code
        // (the highest status class is 5xx).
        constexpr int TABLE_SIZE = 600;
    }

    /// @brief Parses status expressions from the configuration and evaluates failure conditions.
    class HttpStatusErrors {
    public:
        explicit HttpStatusErrors(const std::vector<std::string>& tokens);
        ~HttpStatusErrors() = default;

        /// @brief Returns whether an HTTP status code should be treated as failure.
        bool isErrorCode(int status_code) const noexcept;

    private:
        // Configured tokens in [0, 599] are flattened into a bit table at
        // construction, so the common per-span check is a bounds check plus a
        // constant-time bit lookup. Explicit codes outside that range use the
        // small fallback vector.
        std::bitset<http_status::TABLE_SIZE> error_codes_{};
        std::vector<int> extra_codes_{};  // configured codes outside [0, TABLE_SIZE)
    };

    /// @brief Captures selected HTTP headers and appends them to span annotations.
    class HttpHeaderRecorder {
    public:
        HttpHeaderRecorder(int anno_key, std::vector<std::string> cfg);
        ~HttpHeaderRecorder() = default;

        /// @brief Records headers into @p annotation using the config rules.
        void recordHeader(const HeaderReader& header, PinpointAnnotation* annotation);
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

        /// @brief True when the pattern list matches @p url, i.e. it should be
        ///        ignored for statistics.
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

        // Per-thread scratch, teardown-safe: a host thread_local destructor
        // that records a final span during thread exit reaches isFiltered()
        // exactly then (AgentImpl::NewSpan runs the URL filter) — see
        // thread_local_lazy in utility.h.
        static MatchScratch& match_scratch();

        std::vector<CompiledPattern> patterns_;

        static CompiledPattern compilePattern(const std::string& pattern);
        static bool ant_match(const CompiledPattern& pattern, std::string_view url, MatchScratch& scratch);
    };

    /// @brief Filters HTTP methods according to a configuration whitelist or blacklist.
    class HttpMethodFilter {
    public:
        HttpMethodFilter(std::vector<std::string> cfg);
        ~HttpMethodFilter() = default;

        /// @brief True when the filter rules match @p method (e.g. GET, POST).
        bool isFiltered(std::string_view method) const;
    private:
        std::vector<std::string> methods_;
    };

    /// @brief Utility functions for HTTP tracing.
    class HttpTracerUtil {
    public:
        /// @brief Extracts the real remote address, checking X-Forwarded-For
        ///        and X-Real-Ip before falling back to @p remote_addr.
        static std::string getRemoteAddr(const HeaderReader& reader, std::string_view remote_addr);

        /// @brief Parses the Pinpoint proxy headers (Apache, Nginx or App) and
        ///        records their timing and load information as annotations.
        static void setProxyHeader(const HeaderReader& reader, PinpointAnnotation* annotation);
    };

}
