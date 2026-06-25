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
#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "absl/strings/str_split.h"
#include "logging.h"
#include "pinpoint/tracer.h"
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

    HttpUrlFilter::HttpUrlFilter(const std::vector<std::string>& cfg)
        : patterns_(cfg) {}

    bool HttpUrlFilter::isFiltered(std::string_view url) const {
        for (const auto& pattern : patterns_) {
            if (ant_match(pattern, url)) {
                return true;
            }
        }
        return false;
    }

    bool HttpUrlFilter::ant_match(std::string_view pattern, std::string_view url) {
        const size_t P = pattern.size();
        const size_t U = url.size();

        // Bottom-up dynamic programming over (pattern index, url index). This
        // replaces the previous recursive '**' handling, which retried every
        // url suffix and cost O(n^k) for a pattern with k '**' wildcards on a
        // non-matching url — a remote CPU amplifier since the url is supplied
        // by the caller on every traced request. Each (pi, ui) state is now
        // visited once, so matching is O(pattern_len * url_len): linear in the
        // attacker-controlled url length for a fixed configured pattern.
        //
        // dp[pi][ui] == "pattern[pi:] matches url[ui:]". Semantics preserved
        // from the original matcher:
        //   '*'  matches zero or more characters within a single path segment
        //        (never crosses '/').
        //   '**' matches zero or more characters across segments (may cross
        //        '/'), and absorbs one immediately following '/' in the
        //        pattern — except when the url is already exhausted, where a
        //        trailing '/' after '**' must still match literally and so fails.
        //   '?'  matches exactly one character (including '/').
        //   any other character matches itself literally.
        std::vector<char> dp((P + 1) * (U + 1), 0);
        const auto at = [U](size_t pi, size_t ui) { return pi * (U + 1) + ui; };

        // Base row: an empty remaining pattern matches only an empty remaining url.
        for (size_t ui = 0; ui <= U; ++ui) {
            dp[at(P, ui)] = (ui == U) ? 1 : 0;
        }

        for (size_t pi = P; pi-- > 0;) {
            const char c = pattern[pi];

            // ui == U (url exhausted): only a run of trailing '*' still matches.
            // A trailing '/' after '**' is intentionally not absorbed here.
            dp[at(pi, U)] = (c == '*' && dp[at(pi + 1, U)]) ? 1 : 0;

            if (c == '*' && pi + 1 < P && pattern[pi + 1] == '*') {
                // '**': skip the two stars and one optional following '/'.
                size_t npi = pi + 2;
                if (npi < P && pattern[npi] == '/') {
                    ++npi;
                }
                // dp[pi][ui] = OR over k in [ui, U] of dp[npi][k]. The running
                // OR seeds from dp[npi][U] (not dp[pi][U], which holds the
                // distinct url-exhausted value computed above).
                char running = dp[at(npi, U)];
                for (size_t ui = U; ui-- > 0;) {
                    running = (running || dp[at(npi, ui)]) ? 1 : 0;
                    dp[at(pi, ui)] = running;
                }
            } else if (c == '*') {
                // single '*': extend within a segment, never crossing '/'.
                for (size_t ui = U; ui-- > 0;) {
                    char v = dp[at(pi + 1, ui)];
                    if (!v && url[ui] != '/') {
                        v = dp[at(pi, ui + 1)];
                    }
                    dp[at(pi, ui)] = v;
                }
            } else if (c == '?') {
                // '?': match exactly one character (including '/').
                for (size_t ui = U; ui-- > 0;) {
                    dp[at(pi, ui)] = dp[at(pi + 1, ui + 1)];
                }
            } else {
                // literal character.
                for (size_t ui = U; ui-- > 0;) {
                    dp[at(pi, ui)] = (url[ui] == c) ? dp[at(pi + 1, ui + 1)] : 0;
                }
            }
        }

        return dp[at(0, 0)] != 0;
    }

    HttpMethodFilter::HttpMethodFilter(const std::vector<std::string>& cfg) {
        for (const auto& m : cfg) {
            std::string upper(m);
            std::transform(upper.begin(), upper.end(), upper.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            methods_.insert(std::move(upper));
        }
    }

    bool HttpMethodFilter::isFiltered(std::string_view method) const {
        std::string upper(method);
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        return methods_.count(upper) > 0;
    }

    namespace {
        /// @brief Extracts and trims the first IP address from a comma-separated list.
        /// @param value Header value that may contain comma-separated IP addresses.
        /// @return The first IP address with whitespace trimmed, or empty string if parsing fails.
        std::string extractFirstIp(const std::string& value) {
            if (value.empty()) {
                return "";
            }

            // Extract first IP from comma-separated list
            auto comma_pos = value.find(',');
            std::string_view first_ip = (comma_pos != std::string::npos) 
                ? std::string_view(value).substr(0, comma_pos)
                : std::string_view(value);

            // Trim leading/trailing whitespace
            auto start = first_ip.find_first_not_of(" \t");
            auto end = first_ip.find_last_not_of(" \t");
            
            if (start != std::string::npos && end != std::string::npos) {
                return std::string(first_ip.substr(start, end - start + 1));
            }

            return "";
        }
    }

    std::string HttpTracerUtil::getRemoteAddr(const HeaderReader& reader, std::string_view remote_addr) {
        // Check X-Forwarded-For header
        if (auto xff = reader.Get("X-Forwarded-For"); xff.has_value()) {
            auto ip = extractFirstIp(xff.value());
            if (!ip.empty()) {
                return ip;
            }
        }

        // Check X-Real-Ip header
        if (auto xri = reader.Get("X-Real-Ip"); xri.has_value()) {
            auto ip = extractFirstIp(xri.value());
            if (!ip.empty()) {
                return ip;
            }
        }

        // No proxy headers, extract IP from RemoteAddr (may include port)
        std::string addr_str(remote_addr);
        
        // Handle IPv6 addresses enclosed in brackets [::]:port
        if (!addr_str.empty() && addr_str[0] == '[') {
            auto bracket_end = addr_str.find(']');
            if (bracket_end != std::string::npos) {
                // Extract IPv6 address with brackets
                return addr_str.substr(0, bracket_end + 1);
            }
        }
        
        // Try to split host:port for IPv4
        auto colon_pos = addr_str.rfind(':');
        if (colon_pos != std::string::npos) {
            // Check if this is IPv6 without brackets (contains multiple colons)
            auto first_colon = addr_str.find(':');
            if (first_colon != colon_pos) {
                // Multiple colons, likely IPv6 without brackets - return as is
                return addr_str;
            }
            // Single colon, extract host part (IPv4:port)
            return addr_str.substr(0, colon_pos);
        }

        return addr_str;
    }

    namespace {
        /// @brief Parses space-separated key=value pairs from a header string.
        /// @param value Header value containing key=value pairs separated by spaces.
        /// @return Map of key-value pairs.
        std::map<std::string, std::string> parseKeyValuePairs(const std::string& value) {
            std::map<std::string, std::string> result;
            
            // Split by space and parse each key=value pair
            for (const auto& pair : absl::StrSplit(value, ' ', absl::SkipEmpty())) {
                std::vector<std::string> kv = absl::StrSplit(pair, absl::MaxSplits('=', 1));
                if (kv.size() == 2) {
                    result[kv[0]] = kv[1];
                }
            }
            
            return result;
        }

        /// @brief Extracts and parses a value from key-value pairs with optional transformation.
        /// @tparam T Target type for the output value.
        /// @tparam ParseFunc Parser function type (e.g., stoi_, stoll_, stod_).
        /// @tparam TransformFunc Transformation function type.
        /// @param pairs Map of key-value pairs.
        /// @param key Key to look up.
        /// @param parser Parser function that returns std::optional<T>.
        /// @param transform Transformation function applied to parsed value.
        /// @param output Reference to store the result.
        /// @return true if value was found and parsed successfully, false otherwise.
        template<typename T, typename ParseFunc, typename TransformFunc>
        bool parseAndSet(const std::map<std::string, std::string>& pairs,
                        const std::string& key,
                        ParseFunc parser,
                        TransformFunc transform,
                        T& output) {
            auto it = pairs.find(key);
            if (it != pairs.end()) {
                auto parsed = parser(it->second);
                if (parsed.has_value()) {
                    output = transform(parsed.value());
                    return true;
                }
            }
            return false;
        }

        /// @brief Overload without transformation (identity function).
        template<typename T, typename ParseFunc>
        bool parseAndSet(const std::map<std::string, std::string>& pairs,
                        const std::string& key,
                        ParseFunc parser,
                        T& output) {
            return parseAndSet(pairs, key, parser, [](const auto& v) { return v; }, output);
        }

        /// @brief Extracts a string value directly without parsing.
        bool extractString(const std::map<std::string, std::string>& pairs,
                          const std::string& key,
                          std::string& output) {
            auto it = pairs.find(key);
            if (it != pairs.end()) {
                output = it->second;
                return true;
            }
            return false;
        }
    }

    void HttpTracerUtil::setProxyHeader(const HeaderReader& reader, const AnnotationPtr& annotation) {
        int64_t received_time = 0;
        int duration_time = 0;
        int idle_percent = 0;
        int busy_percent = 0;
        int32_t code = 0;
        std::string app;

        // Check Pinpoint-ProxyApache header
        if (auto apache = reader.Get("Pinpoint-ProxyApache"); apache.has_value()) {
            auto pairs = parseKeyValuePairs(apache.value());
            
            parseAndSet(pairs, "t", stoll_, [](int64_t v) { return v / 1000; }, received_time);
            parseAndSet(pairs, "D", stoi_, duration_time);
            parseAndSet(pairs, "i", stoi_, idle_percent);
            parseAndSet(pairs, "b", stoi_, busy_percent);
            code = 3;
        }
        // Check Pinpoint-ProxyNginx header
        else if (auto nginx = reader.Get("Pinpoint-ProxyNginx"); nginx.has_value()) {
            auto pairs = parseKeyValuePairs(nginx.value());
            
            parseAndSet(pairs, "t", stod_, [](double v) { return static_cast<int64_t>(v * 1000); }, received_time);
            parseAndSet(pairs, "D", stoi_, duration_time);
            code = 2;
        }
        // Check Pinpoint-ProxyApp header
        else if (auto proxy_app = reader.Get("Pinpoint-ProxyApp"); proxy_app.has_value()) {
            auto pairs = parseKeyValuePairs(proxy_app.value());
            
            parseAndSet(pairs, "t", stoll_, received_time);
            extractString(pairs, "app", app);
            code = 1;
        }

        // If any proxy header was found, add annotation
        if (code > 0) {
            annotation->AppendLongIntIntByteByteString(
                ANNOTATION_HTTP_PROXY_HEADER,
                received_time,
                code,
                static_cast<int32_t>(duration_time),
                static_cast<int32_t>(idle_percent),
                static_cast<int32_t>(busy_percent),
                app
            );
        }
    }

    namespace helper {
        void TraceHttpServerRequest(SpanPtr span, std::string_view remote_addr, std::string_view endpoint, HeaderReader& request_reader) {
            if (!span) {
                return;
            }
            std::string r_addr = HttpTracerUtil::getRemoteAddr(request_reader, remote_addr);
            span->SetRemoteAddress(r_addr);
            span->SetEndPoint(endpoint);

            HttpTracerUtil::setProxyHeader(request_reader, span->GetAnnotations());
            span->RecordHeader(HTTP_REQUEST, request_reader);
        }

        void TraceHttpServerRequest(SpanPtr span, std::string_view remote_addr, std::string_view endpoint, HeaderReader& request_reader, HeaderReader& cookie_reader) {
            if (!span) {
                return;
            }
            TraceHttpServerRequest(span, remote_addr, endpoint, request_reader);
            span->RecordHeader(HTTP_COOKIE, cookie_reader);
        }

        void TraceHttpServerResponse(SpanPtr span, std::string_view url_pattern, std::string_view method, int status_code, HeaderReader& response_reader){
            if (!span) {
                return;
            }
            span->SetStatusCode(status_code);
            span->SetUrlStat(url_pattern, method, status_code);
            span->RecordHeader(HTTP_RESPONSE, response_reader);
        }

        void TraceHttpClientRequest(SpanEventPtr span_event, std::string_view host, std::string_view url, HeaderReader& request_reader) {
            if (!span_event) {
                return;
            }
            span_event->SetServiceType(SERVICE_TYPE_CPP_HTTP_CLIENT);
            span_event->SetEndPoint(host);
            span_event->SetDestination(host);
            span_event->GetAnnotations()->AppendString(ANNOTATION_HTTP_URL, url);
            span_event->RecordHeader(HTTP_REQUEST, request_reader);
        }

        void TraceHttpClientRequest(SpanEventPtr span_event, std::string_view host, std::string_view url, HeaderReader& request_reader, HeaderReader& cookie_reader) {
            if (!span_event) {
                return;
            }
            TraceHttpClientRequest(span_event, host, url, request_reader);
            span_event->RecordHeader(HTTP_COOKIE, cookie_reader);
        }

        void TraceHttpClientResponse(SpanEventPtr span_event, int status_code, HeaderReader& response_reader) {
            if (!span_event) {
                return;
            }
            span_event->GetAnnotations()->AppendInt(ANNOTATION_HTTP_STATUS_CODE, status_code);
            span_event->RecordHeader(HTTP_RESPONSE, response_reader);
        }
    } // namespace helper
} // namespace pinpoint
