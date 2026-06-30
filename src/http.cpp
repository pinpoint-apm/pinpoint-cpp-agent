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
#include <map>
#include <string>
#include <vector>

#include "absl/strings/str_split.h"
#include "absl/strings/numbers.h"
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
        if (cfg_.empty() || annotation == nullptr) {
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

    namespace {
        bool contains_wildcard(std::string_view value) {
            return value.find('*') != std::string_view::npos || value.find('?') != std::string_view::npos;
        }

        bool starts_with(std::string_view value, std::string_view prefix) {
            return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
        }

        std::string to_string(std::string_view value) {
            return std::string(value.data(), value.size());
        }

        std::string_view as_view(const std::string& value) {
            return std::string_view(value.data(), value.size());
        }
    }

    HttpUrlFilter::HttpUrlFilter(const std::vector<std::string>& cfg) {
        patterns_.reserve(cfg.size());
        for (const auto& pattern : cfg) {
            patterns_.push_back(compilePattern(pattern));
        }
    }

    bool HttpUrlFilter::isFiltered(std::string_view url) const {
        MatchScratch scratch;
        for (const auto& pattern : patterns_) {
            switch (pattern.kind) {
            case PatternKind::Exact:
                if (url.size() == pattern.pattern.size() && url.compare(as_view(pattern.pattern)) == 0) {
                    return true;
                }
                break;
            case PatternKind::Prefix:
                if (starts_with(url, as_view(pattern.literal_prefix))) {
                    return true;
                }
                break;
            case PatternKind::SegmentPrefix:
                if (starts_with(url, as_view(pattern.literal_prefix)) &&
                    url.find('/', pattern.literal_prefix.size()) == std::string_view::npos) {
                    return true;
                }
                break;
            case PatternKind::Ant: {
                if (ant_match(pattern, url, scratch)) {
                    return true;
                }
                break;
            }
            }
        }
        return false;
    }

    HttpUrlFilter::CompiledPattern HttpUrlFilter::compilePattern(const std::string& pattern) {
        CompiledPattern compiled;
        compiled.pattern = pattern;

        const std::string_view pattern_view = as_view(compiled.pattern);
        if (!contains_wildcard(pattern_view)) {
            compiled.kind = PatternKind::Exact;
            compiled.literal_prefix = compiled.pattern;
            compiled.min_length = compiled.pattern.size();
            return compiled;
        }

        if (pattern_view.size() >= 2 &&
            pattern_view[pattern_view.size() - 1] == '*' &&
            pattern_view[pattern_view.size() - 2] == '*') {
            const auto prefix = pattern_view.substr(0, pattern_view.size() - 2);
            if (!contains_wildcard(prefix)) {
                compiled.kind = PatternKind::Prefix;
                compiled.literal_prefix = to_string(prefix);
                return compiled;
            }
        }

        if (!pattern_view.empty() &&
            pattern_view.back() == '*' &&
            (pattern_view.size() == 1 || pattern_view[pattern_view.size() - 2] != '*')) {
            const auto prefix = pattern_view.substr(0, pattern_view.size() - 1);
            if (!contains_wildcard(prefix)) {
                compiled.kind = PatternKind::SegmentPrefix;
                compiled.literal_prefix = to_string(prefix);
                return compiled;
            }
        }

        compiled.kind = PatternKind::Ant;
        const auto prefix_end = pattern_view.find_first_of("*?");
        if (prefix_end != std::string_view::npos && prefix_end > 0) {
            compiled.literal_prefix = to_string(pattern_view.substr(0, prefix_end));
        }

        for (size_t i = 0; i < pattern_view.size();) {
            const char c = pattern_view[i];
            if (c == '*' && i + 1 < pattern_view.size() && pattern_view[i + 1] == '*') {
                if (i + 2 < pattern_view.size() && pattern_view[i + 2] == '/') {
                    compiled.tokens.push_back({TokenKind::DoubleStarSlash, '\0'});
                    i += 3;
                } else {
                    compiled.tokens.push_back({TokenKind::DoubleStar, '\0'});
                    i += 2;
                }
            } else if (c == '*') {
                compiled.tokens.push_back({TokenKind::Star, '\0'});
                ++i;
            } else if (c == '?') {
                compiled.tokens.push_back({TokenKind::Question, '\0'});
                ++compiled.min_length;
                ++i;
            } else {
                compiled.tokens.push_back({TokenKind::Literal, c});
                ++compiled.min_length;
                ++i;
            }
        }

        return compiled;
    }

    bool HttpUrlFilter::ant_match(const CompiledPattern& pattern, std::string_view url, MatchScratch& scratch) {
        const size_t U = url.size();

        if (U < pattern.min_length) {
            return false;
        }
        if (!pattern.literal_prefix.empty() && !starts_with(url, as_view(pattern.literal_prefix))) {
            return false;
        }

        // Two-row suffix DP. The compiled token stream preserves the existing
        // '**/' behavior: the slash is skipped only while URL input remains.
        scratch.next.assign(U + 1, 0);
        scratch.current.resize(U + 1);
        scratch.next[U] = 1;

        for (size_t ti = pattern.tokens.size(); ti-- > 0;) {
            const auto& token = pattern.tokens[ti];
            switch (token.kind) {
            case TokenKind::Literal:
                scratch.current[U] = 0;
                for (size_t ui = U; ui-- > 0;) {
                    scratch.current[ui] =
                        (url[ui] == token.value && scratch.next[ui + 1]) ? 1 : 0;
                }
                break;
            case TokenKind::Question:
                scratch.current[U] = 0;
                for (size_t ui = U; ui-- > 0;) {
                    scratch.current[ui] = scratch.next[ui + 1];
                }
                break;
            case TokenKind::Star:
                scratch.current[U] = scratch.next[U];
                for (size_t ui = U; ui-- > 0;) {
                    char value = scratch.next[ui];
                    if (!value && url[ui] != '/') {
                        value = scratch.current[ui + 1];
                    }
                    scratch.current[ui] = value;
                }
                break;
            case TokenKind::DoubleStar:
            case TokenKind::DoubleStarSlash: {
                scratch.current[U] = (token.kind == TokenKind::DoubleStar) ? scratch.next[U] : 0;
                char running = scratch.next[U];
                for (size_t ui = U; ui-- > 0;) {
                    running = (running || scratch.next[ui]) ? 1 : 0;
                    scratch.current[ui] = running;
                }
                break;
            }
            }

            scratch.next.swap(scratch.current);
        }

        return scratch.next[0] != 0;
    }

    bool HttpUrlFilter::ant_match(std::string_view pattern, std::string_view url) {
        const auto compiled = compilePattern(to_string(pattern));
        switch (compiled.kind) {
        case PatternKind::Exact:
            return url.size() == compiled.pattern.size() && url.compare(as_view(compiled.pattern)) == 0;
        case PatternKind::Prefix:
            return starts_with(url, as_view(compiled.literal_prefix));
        case PatternKind::SegmentPrefix:
            return starts_with(url, as_view(compiled.literal_prefix)) &&
                url.find('/', compiled.literal_prefix.size()) == std::string_view::npos;
        case PatternKind::Ant: {
            MatchScratch scratch;
            return ant_match(compiled, url, scratch);
        }
        }

        return false;
    }

    HttpMethodFilter::HttpMethodFilter(const std::vector<std::string>& cfg)
        : methods_(cfg) {}

    bool HttpMethodFilter::isFiltered(std::string_view method) const {
        return std::any_of(methods_.begin(), methods_.end(),
            [method](const auto& filtered_method) {
                return compare_string(method, filtered_method);
            });
    }

    namespace {
        /// @brief Extracts and trims the first IP address from a comma-separated list.
        /// @param value Header value that may contain comma-separated IP addresses.
        /// @return The first IP address with whitespace trimmed.
        std::string_view extractFirstIp(std::string_view value) {
            if (value.empty()) {
                return {};
            }

            // Extract first IP from comma-separated list
            auto comma_pos = value.find(',');
            std::string_view first_ip = (comma_pos != std::string::npos) 
                ? value.substr(0, comma_pos)
                : value;

            // Trim leading/trailing whitespace
            auto start = first_ip.find_first_not_of(" \t");
            auto end = first_ip.find_last_not_of(" \t");
            
            if (start != std::string::npos && end != std::string::npos) {
                return first_ip.substr(start, end - start + 1);
            }

            return {};
        }
    }

    std::string HttpTracerUtil::getRemoteAddr(const HeaderReader& reader, std::string_view remote_addr) {
        // Check X-Forwarded-For header
        if (auto xff = reader.Get("X-Forwarded-For"); xff.has_value()) {
            auto ip = extractFirstIp(xff.value());
            if (!ip.empty()) {
                return std::string(ip);
            }
        }

        // Check X-Real-Ip header
        if (auto xri = reader.Get("X-Real-Ip"); xri.has_value()) {
            auto ip = extractFirstIp(xri.value());
            if (!ip.empty()) {
                return std::string(ip);
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
        struct ProxyHeaderValues {
            std::string_view t_val;
            std::string_view D_val;
            std::string_view i_val;
            std::string_view b_val;
            std::string_view app_val;
        };

        ProxyHeaderValues parseProxyHeaderInline(std::string_view value) {
            ProxyHeaderValues result{};
            size_t pos = 0;
            const size_t len = value.size();
            while (pos < len) {
                pos = value.find_first_not_of(' ', pos);
                if (pos == std::string_view::npos) {
                    break;
                }
                
                size_t eq_pos = value.find('=', pos);
                if (eq_pos == std::string_view::npos) {
                    break;
                }
                
                std::string_view key = value.substr(pos, eq_pos - pos);
                
                size_t val_end = value.find(' ', eq_pos + 1);
                if (val_end == std::string_view::npos) {
                    val_end = len;
                }
                
                std::string_view val = value.substr(eq_pos + 1, val_end - eq_pos - 1);
                
                if (key == "t") {
                    result.t_val = val;
                } else if (key == "D") {
                    result.D_val = val;
                } else if (key == "i") {
                    result.i_val = val;
                } else if (key == "b") {
                    result.b_val = val;
                } else if (key == "app") {
                    result.app_val = val;
                }
                
                pos = val_end;
            }
            return result;
        }
    }

    void HttpTracerUtil::setProxyHeader(const HeaderReader& reader, AnnotationPtr annotation) {
        if (annotation == nullptr) {
            return;
        }

        int64_t received_time = 0;
        int duration_time = 0;
        int idle_percent = 0;
        int busy_percent = 0;
        int32_t code = 0;
        std::string app;

        // Check Pinpoint-ProxyApache header
        if (auto apache = reader.Get("Pinpoint-ProxyApache"); apache.has_value()) {
            auto values = parseProxyHeaderInline(apache.value());
            if (!values.t_val.empty()) {
                int64_t t = 0;
                if (absl::SimpleAtoi(values.t_val, &t)) received_time = t / 1000;
            }
            if (!values.D_val.empty() && !absl::SimpleAtoi(values.D_val, &duration_time)) {
                duration_time = 0;
            }
            if (!values.i_val.empty() && !absl::SimpleAtoi(values.i_val, &idle_percent)) {
                idle_percent = 0;
            }
            if (!values.b_val.empty() && !absl::SimpleAtoi(values.b_val, &busy_percent)) {
                busy_percent = 0;
            }
            code = 3;
        }
        // Check Pinpoint-ProxyNginx header
        else if (auto nginx = reader.Get("Pinpoint-ProxyNginx"); nginx.has_value()) {
            auto values = parseProxyHeaderInline(nginx.value());
            if (!values.t_val.empty()) {
                double t = 0.0;
                if (absl::SimpleAtod(values.t_val, &t)) received_time = static_cast<int64_t>(t * 1000);
            }
            if (!values.D_val.empty() && !absl::SimpleAtoi(values.D_val, &duration_time)) {
                duration_time = 0;
            }
            code = 2;
        }
        // Check Pinpoint-ProxyApp header
        else if (auto proxy_app = reader.Get("Pinpoint-ProxyApp"); proxy_app.has_value()) {
            auto values = parseProxyHeaderInline(proxy_app.value());
            if (!values.t_val.empty()) {
                if (!absl::SimpleAtoi(values.t_val, &received_time)) {
                    received_time = 0;
                }
            }
            if (!values.app_val.empty()) app = std::string(values.app_val);
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
