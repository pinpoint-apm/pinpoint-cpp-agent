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

#include "absl/strings/str_split.h"
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
            std::string r_addr = HttpTracerUtil::getRemoteAddr(request_reader, remote_addr);
            span->SetRemoteAddress(r_addr);
            span->SetEndPoint(endpoint);

            HttpTracerUtil::setProxyHeader(request_reader, span->GetAnnotations());
            span->RecordHeader(HTTP_REQUEST, request_reader);
        }

        void TraceHttpServerRequest(SpanPtr span, std::string_view remote_addr, std::string_view endpoint, HeaderReader& request_reader, HeaderReader& cookie_reader) {
            TraceHttpServerRequest(span, remote_addr, endpoint, request_reader);
            span->RecordHeader(HTTP_COOKIE, cookie_reader);
        }

        void TraceHttpServerResponse(SpanPtr span, std::string_view url_pattern, std::string_view method, int status_code, HeaderReader& response_reader){
            span->SetStatusCode(status_code);
            span->SetUrlStat(url_pattern, method, status_code);
            span->RecordHeader(HTTP_RESPONSE, response_reader);
        }

        void TraceHttpClientRequest(SpanEventPtr span_event, std::string_view host, std::string_view url, HeaderReader& request_reader) {
            span_event->SetEndPoint(host);
            span_event->SetDestination(host);
            span_event->GetAnnotations()->AppendString(ANNOTATION_HTTP_URL, url);
            span_event->RecordHeader(HTTP_REQUEST, request_reader);
        }

        void TraceHttpClientRequest(SpanEventPtr span_event, std::string_view host, std::string_view url, HeaderReader& request_reader, HeaderReader& cookie_reader) {
            TraceHttpClientRequest(span_event, host, url, request_reader);
            span_event->RecordHeader(HTTP_COOKIE, cookie_reader);
        }

        void TraceHttpClientResponse(SpanEventPtr span_event, int status_code, HeaderReader& response_reader) {
            span_event->GetAnnotations()->AppendInt(ANNOTATION_HTTP_STATUS_CODE, status_code);
            span_event->RecordHeader(HTTP_RESPONSE, response_reader);
        }
    } // namespace helper
} // namespace pinpoint
