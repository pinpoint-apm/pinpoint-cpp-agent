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
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "absl/strings/numbers.h"
#include "annotation.h"
#include "logging.h"
#include "pinpoint/tracer.h"
#include "span.h"
#include "utility.h"
#include "http.h"

namespace pinpoint {

    HttpStatusErrors::HttpStatusErrors(const std::vector<std::string>& tokens) {
        const auto set_range = [this](int min, int max) {
            for (int code = min; code <= max; ++code) {
                error_codes_.set(code);
            }
        };
        for (const auto& token : tokens){
            // "1xx".."5xx" (case-insensitive) selects a whole status class.
            if (token.size() == 3 && '1' <= token[0] && token[0] <= '5' &&
                (token[1] == 'x' || token[1] == 'X') &&
                (token[2] == 'x' || token[2] == 'X')) {
                const int base = (token[0] - '0') * 100;
                set_range(base, base + 99);
            } else {
                auto result = stoi_(token);
                if (result.has_value() && 0 <= result.value() &&
                    result.value() < http_status::TABLE_SIZE) {
                    error_codes_.set(result.value());
                } else {
                    LOG_WARN("ignoring invalid http status error token: {}", token);
                }
            }
        }
    }

    bool HttpStatusErrors::isErrorCode(int status_code) const noexcept {
        return 0 <= status_code && status_code < http_status::TABLE_SIZE &&
               error_codes_[status_code];
    }

    HttpHeaderRecorder::HttpHeaderRecorder(int anno_key, std::vector<std::string> cfg) 
        : anno_key_(anno_key), 
          cfg_(std::move(cfg)),
          dump_all_headers_(cfg_.size() == 1 && absl::EqualsIgnoreCase(cfg_[0], "HEADERS-ALL")) {
    }

    void HttpHeaderRecorder::recordHeader(const HeaderReader& header, PinpointAnnotation* annotation) {
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
    }

    HttpUrlFilter::HttpUrlFilter(const std::vector<std::string>& cfg) {
        patterns_.reserve(cfg.size());
        for (const auto& pattern : cfg) {
            patterns_.push_back(compilePattern(pattern));
        }
    }

    HttpUrlFilter::MatchScratch& HttpUrlFilter::match_scratch() {
        // A teardown re-entry leaks one small buffer pair per thread that
        // runs the URL filter again during its own exit (thread_local_lazy).
        return thread_local_lazy<MatchScratch>([] { return new MatchScratch(); });
    }

    bool HttpUrlFilter::isFiltered(std::string_view url) const {
        for (const auto& pattern : patterns_) {
            switch (pattern.kind) {
            case PatternKind::Exact:
                if (url == pattern.pattern) {
                    return true;
                }
                break;
            case PatternKind::Prefix:
                if (absl::StartsWith(url, pattern.literal_prefix)) {
                    return true;
                }
                break;
            case PatternKind::SegmentPrefix:
                if (absl::StartsWith(url, pattern.literal_prefix) &&
                    url.find('/', pattern.literal_prefix.size()) == std::string_view::npos) {
                    return true;
                }
                break;
            case PatternKind::Ant: {
                // Fetched per Ant pattern, not once per call: reuse the
                // per-thread DP rows across calls (matching avoids further
                // vector growth while the URL fits the retained capacity),
                // without paying the slot's one heap allocation on threads
                // whose patterns never reach an Ant match.
                if (ant_match(pattern, url, match_scratch())) {
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

        const std::string_view pattern_view = compiled.pattern;
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
                compiled.literal_prefix = std::string(prefix);
                return compiled;
            }
        }

        if (!pattern_view.empty() &&
            pattern_view.back() == '*' &&
            (pattern_view.size() == 1 || pattern_view[pattern_view.size() - 2] != '*')) {
            const auto prefix = pattern_view.substr(0, pattern_view.size() - 1);
            if (!contains_wildcard(prefix)) {
                compiled.kind = PatternKind::SegmentPrefix;
                compiled.literal_prefix = std::string(prefix);
                return compiled;
            }
        }

        compiled.kind = PatternKind::Ant;
        const auto prefix_end = pattern_view.find_first_of("*?");
        if (prefix_end != std::string_view::npos && prefix_end > 0) {
            compiled.literal_prefix = std::string(pattern_view.substr(0, prefix_end));
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
        if (!pattern.literal_prefix.empty() && !absl::StartsWith(url, pattern.literal_prefix)) {
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
                // Ant semantics: '?' matches one character but never the
                // path separator, same as '*'.
                scratch.current[U] = 0;
                for (size_t ui = U; ui-- > 0;) {
                    scratch.current[ui] =
                        (url[ui] != '/' && scratch.next[ui + 1]) ? 1 : 0;
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

    HttpMethodFilter::HttpMethodFilter(std::vector<std::string> cfg)
        : methods_(std::move(cfg)) {}

    bool HttpMethodFilter::isFiltered(std::string_view method) const {
        return std::any_of(methods_.begin(), methods_.end(),
            [method](const auto& filtered_method) {
                return absl::EqualsIgnoreCase(method, filtered_method);
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
        // Canonical mixed-case lookups, here and in setProxyHeader/
        // recordHeader below: correct only under the Get contract in
        // pinpoint/tracer.h, which requires HTTP-backed readers to match
        // header names case-insensitively (HTTP/2/3 deliver them lowercase).
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

        // No proxy headers, extract IP from RemoteAddr (may include port).
        // Parse on the view and materialize the result string exactly once —
        // this is the common direct-connection path, and the previous code
        // copied remote_addr into a std::string and then substr()'d it, up to
        // two heap allocations where one suffices.

        // Handle IPv6 addresses enclosed in brackets [::]:port
        if (!remote_addr.empty() && remote_addr[0] == '[') {
            auto bracket_end = remote_addr.find(']');
            if (bracket_end != std::string_view::npos) {
                // Extract IPv6 address with brackets
                return std::string(remote_addr.substr(0, bracket_end + 1));
            }
        }

        // Try to split host:port for IPv4
        auto colon_pos = remote_addr.rfind(':');
        if (colon_pos != std::string_view::npos) {
            // Check if this is IPv6 without brackets (contains multiple colons)
            auto first_colon = remote_addr.find(':');
            if (first_colon != colon_pos) {
                // Multiple colons, likely IPv6 without brackets - return as is
                return std::string(remote_addr);
            }
            // Single colon, extract host part (IPv4:port)
            return std::string(remote_addr.substr(0, colon_pos));
        }

        return std::string(remote_addr);
    }

    namespace {
        // Java ProxyRequestAnnotationFactory.APP_MAX_LENGTH: the cap the
        // annotation's app field is abbreviated to. The App parser validates
        // its own value against the stricter kProxyAppMaxIdLength below, so
        // this only ever bites the user parser, whose app field is the matched
        // header name. Deliberately a plain cut: Java's
        // StringUtils.abbreviate(app, 32) also appends "...(<original
        // length>)", which utility.h's abbreviateErrorString reproduces if
        // that parity is ever wanted here.
        constexpr size_t kProxyAppMaxLength = 32;

        // Java AppRequestParser: IdValidateUtils.validateId(app, 30). A value
        // failing either half of that check discards the whole header.
        constexpr size_t kProxyAppMaxIdLength = 30;

        constexpr std::string_view kProxyHeaderApache = "Pinpoint-ProxyApache";
        constexpr std::string_view kProxyHeaderNginx = "Pinpoint-ProxyNginx";
        constexpr std::string_view kProxyHeaderApp = "Pinpoint-ProxyApp";

        // Java ProxyRequestType.getCode() per request type; the collector and
        // web UI key the annotation's display name off these.
        constexpr int32_t kProxyCodeApp = 1;
        constexpr int32_t kProxyCodeNginx = 2;
        constexpr int32_t kProxyCodeApache = 3;
        constexpr int32_t kProxyCodeUser = 4;

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

                size_t token_end = value.find(' ', pos);
                if (token_end == std::string_view::npos) {
                    token_end = len;
                }

                // The '=' must belong to the current space-delimited token;
                // otherwise a malformed token would swallow its neighbors.
                size_t eq_pos = value.find('=', pos);
                if (eq_pos == std::string_view::npos || eq_pos >= token_end) {
                    pos = token_end;
                    continue;
                }

                std::string_view key = value.substr(pos, eq_pos - pos);
                std::string_view val = value.substr(eq_pos + 1, token_end - eq_pos - 1);

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

                pos = token_end;
            }
            return result;
        }

        /// @brief Digits only, no sign and no whitespace, overflow-checked.
        ///
        /// Java's NumberUtils.parse* return their default on anything
        /// Long/Integer.parseLong rejects, and every proxy timing field that
        /// survives its own > 0 gate below is unsigned — so refusing a sign
        /// here reaches the same outcome. absl::SimpleAtoi is not used because
        /// it tolerates surrounding whitespace, which Java does not.
        std::optional<int64_t> parseProxyDigits(std::string_view value) {
            if (value.empty()) {
                return std::nullopt;
            }
            int64_t result = 0;
            for (const char c : value) {
                if (c < '0' || c > '9') {
                    return std::nullopt;
                }
                const int digit = c - '0';
                if (result > (std::numeric_limits<int64_t>::max() - digit) / 10) {
                    return std::nullopt;
                }
                result = result * 10 + digit;
            }
            return result;
        }

        /// @brief Parses nginx's "<seconds>.<mmm>" and returns it scaled by
        ///        1000, i.e. "1504230492.763" -> 1504230492763.
        ///
        /// Java (NginxRequestParser.toReceivedTimeMillis /
        /// toDurationTimeMicros) enforces exactly three digits after the last
        /// '.' and converts by deleting that '.', so anything else — an
        /// integer, one or two decimals, more than three — is rejected rather
        /// than approximated. Done as integer arithmetic like Java, not by
        /// parsing a double and scaling: 0.123 has no exact binary
        /// representation, so double(0.123) * 1e6 truncates to 122999.
        std::optional<int64_t> parseProxySecondsWithMillis(std::string_view value) {
            const auto dot = value.rfind('.');
            if (dot == std::string_view::npos || value.size() - dot != 4) {
                return std::nullopt;  // absent, or not the "sec.mmm" format
            }
            int64_t seconds = 0;
            if (dot > 0) {
                // Java's substring(0, 0) leaves the whole part empty and
                // parses just the milliseconds, so ".763" is 763 there too.
                const auto parsed = parseProxyDigits(value.substr(0, dot));
                if (!parsed.has_value()) {
                    return std::nullopt;
                }
                seconds = *parsed;
            }
            const auto millis = parseProxyDigits(value.substr(dot + 1));
            if (!millis.has_value()) {
                return std::nullopt;
            }
            if (seconds > (std::numeric_limits<int64_t>::max() - *millis) / 1000) {
                return std::nullopt;
            }
            return seconds * 1000 + *millis;
        }

        /// @brief Microseconds from an apache/app `D=`, which is already a
        ///        plain microsecond count. 0 when absent or unparseable.
        int32_t parseProxyMicros(std::string_view value) {
            const auto micros = parseProxyDigits(value);
            if (!micros.has_value() || *micros > std::numeric_limits<int32_t>::max()) {
                return 0;
            }
            return static_cast<int32_t>(*micros);
        }

        /// @brief Microseconds from an nginx `D=` ($request_time, seconds with
        ///        three decimals). 0 when absent or not in that format.
        int32_t parseProxyNginxDurationMicros(std::string_view value) {
            const auto millis = parseProxySecondsWithMillis(value);
            // Java multiplies by 1000 into an int, which silently wraps. The
            // wire field is an int32, so an out-of-range product is reported
            // as "no duration" instead of as a wrapped one.
            if (!millis.has_value() || *millis > std::numeric_limits<int32_t>::max() / 1000) {
                return 0;
            }
            return static_cast<int32_t>(*millis * 1000);
        }

        /// @brief Milliseconds from a user proxy header's `t=`.
        ///
        /// Java UserRequestParser.toReceivedTimeMillis: a configured header
        /// may be written by any of the three proxies, so the format is
        /// inferred from the value's own shape rather than from its name.
        int64_t parseProxyUserReceivedTimeMillis(std::string_view value) {
            const size_t length = value.size();
            if (length < 13) {
                return 0;  // shorter than a millisecond epoch timestamp
            }
            if (length >= 16) {
                // apache: microseconds. Dropping the last three digits like
                // Java's substring(0, length - 3), rather than parsing the
                // whole value and dividing, keeps a 20-digit value out of an
                // int64 overflow that Java never hits.
                return parseProxyDigits(value.substr(0, length - 3)).value_or(0);
            }
            if (const auto dot = value.rfind('.'); dot != std::string_view::npos) {
                // nginx: seconds.milliseconds, e.g. 1504230492.763. The
                // seconds part must itself be a full epoch value.
                if (dot < 10) {
                    return 0;
                }
                return parseProxySecondsWithMillis(value).value_or(0);
            }
            // app: milliseconds.
            return parseProxyDigits(value).value_or(0);
        }

        /// @brief Microseconds from a user proxy header's `D=`, nginx's
        ///        fractional seconds when it carries a '.' (Java
        ///        UserRequestParser.toDurationTimeMicros).
        int32_t parseProxyUserDurationMicros(std::string_view value) {
            if (value.find('.') != std::string_view::npos) {
                return parseProxyNginxDurationMicros(value);
            }
            return parseProxyMicros(value);
        }

        /// @brief Java's String.trim() over the ASCII blanks a space-split
        ///        token can still hold (tab, CR), as AppRequestParser applies
        ///        to `app=` before validating it.
        std::string_view trimProxyValue(std::string_view value) {
            const auto start = value.find_first_not_of(" \t\r\n");
            if (start == std::string_view::npos) {
                return {};
            }
            return value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
        }
    }

    void HttpTracerUtil::setProxyHeader(const HeaderReader& reader, PinpointAnnotation* annotation,
                                        const std::vector<std::string>& user_header_names) {
        if (annotation == nullptr) {
            return;
        }

        // Java DefaultProxyRequestRecorder.record runs every parser and
        // records one annotation per *valid* header, so a request that came
        // through both an Apache and an Nginx proxy records two. Hence
        // independent ifs below rather than a first-match if/else chain.
        //
        // Each block also reproduces its parser's `t=` gate: with no `t=`, or
        // one that is not positive, the parser calls setValid(false) and the
        // recorder records nothing. An annotation whose received time is 0 is
        // worse than no annotation — the web UI charts the proxy-to-agent gap
        // from that field, and 0 renders as a gap of five decades.
        //
        // Every block finishes (append included) before the next Get(): a
        // HeaderReader only guarantees a returned view until the next Get() on
        // the same reader, and the values parsed out of a header point into it.
        const auto append = [annotation](const int32_t code, const int64_t received_time,
                                         const int32_t duration_time, const int32_t idle_percent,
                                         const int32_t busy_percent, const std::string_view app) {
            annotation->AppendLongIntIntByteByteString(
                ANNOTATION_HTTP_PROXY_HEADER,
                received_time,
                code,
                duration_time,
                idle_percent,
                busy_percent,
                // utf8SafeCutLength keeps the cut off a multibyte boundary, so
                // the capped name stays valid UTF-8 for the protobuf string
                // field (same reason SQL and callstack truncation use it).
                app.substr(0, utf8SafeCutLength(app, kProxyAppMaxLength))
            );
        };

        // Apache: `t` is microseconds, `D` a plain microsecond count.
        if (const auto apache = reader.Get(kProxyHeaderApache);
                apache.has_value() && !apache->empty()) {
            const auto values = parseProxyHeaderInline(*apache);
            // Java ApacheRequestParser.toReceivedTimeMillis drops the last
            // three digits rather than dividing, and yields nothing for a
            // value of three digits or fewer.
            const int64_t received_time = values.t_val.size() > 3
                ? parseProxyDigits(values.t_val.substr(0, values.t_val.size() - 3)).value_or(0)
                : 0;
            int idle_percent = 0;
            int busy_percent = 0;
            if (!values.i_val.empty() && !absl::SimpleAtoi(values.i_val, &idle_percent)) {
                idle_percent = 0;
            }
            if (!values.b_val.empty() && !absl::SimpleAtoi(values.b_val, &busy_percent)) {
                busy_percent = 0;
            }
            if (received_time > 0) {
                append(kProxyCodeApache, received_time, parseProxyMicros(values.D_val),
                       static_cast<int32_t>(idle_percent), static_cast<int32_t>(busy_percent), {});
            }
        }

        // Nginx: `t` is $msec and `D` is $request_time — both seconds with
        // three decimals, not the integers Apache sends.
        if (const auto nginx = reader.Get(kProxyHeaderNginx);
                nginx.has_value() && !nginx->empty()) {
            const auto values = parseProxyHeaderInline(*nginx);
            const int64_t received_time = parseProxySecondsWithMillis(values.t_val).value_or(0);
            if (received_time > 0) {
                append(kProxyCodeNginx, received_time,
                       parseProxyNginxDurationMicros(values.D_val), 0, 0, {});
            }
        }

        // App: `t` is already milliseconds, and `app` is validated rather than
        // truncated — Java AppRequestParser discards the header when
        // IdValidateUtils.validateId(app, 30) fails, because the app name
        // names a node in the server map and a mangled one invents a node.
        if (const auto proxy_app = reader.Get(kProxyHeaderApp);
                proxy_app.has_value() && !proxy_app->empty()) {
            const auto values = parseProxyHeaderInline(*proxy_app);
            const int64_t received_time = parseProxyDigits(values.t_val).value_or(0);
            const std::string_view app = trimProxyValue(values.app_val);
            const bool app_valid =
                app.empty() || (app.size() <= kProxyAppMaxIdLength && isIdChars(app));
            if (received_time > 0 && app_valid) {
                append(kProxyCodeApp, received_time, 0, 0, 0, app);
            }
        }

        // User: the header names are configuration
        // (Http.Server.ProxyUserHeaderNames, Java's
        // profiler.proxy.http.headers), and the annotation's app field carries
        // the matched header name so the web UI can label the hop.
        for (const auto& name : user_header_names) {
            if (name.empty()) {
                continue;
            }
            const auto user = reader.Get(name);
            if (!user.has_value() || user->empty()) {
                continue;
            }
            const auto values = parseProxyHeaderInline(*user);
            const int64_t received_time = parseProxyUserReceivedTimeMillis(values.t_val);
            if (received_time > 0) {
                append(kProxyCodeUser, received_time,
                       parseProxyUserDurationMicros(values.D_val), 0, 0, name);
            }
        }
    }

    namespace helper {
        namespace {
            // Shared request-recording body taken by const ref so the two
            // public overloads (which take SpanPtr by value — API-fixed) each
            // pay exactly one shared_ptr copy from their caller, instead of
            // the cookie overload copying a second time to forward.
            void traceServerRequest(const SpanPtr& span, std::string_view remote_addr,
                                    std::string_view endpoint, HeaderReader& request_reader) {
                std::string r_addr = HttpTracerUtil::getRemoteAddr(request_reader, remote_addr);
                span->SetRemoteAddress(r_addr);
                span->SetEndPoint(endpoint);

                // The proxy-header annotation uses an internal-only payload
                // format, so it is recorded straight into the recording
                // span's annotation container; noop/unsampled spans have
                // none and record nothing, as before.
                if (auto* impl = dynamic_cast<SpanImpl*>(span.get())) {
                    // Java ServerRequestRecorder.recordParentInfo records
                    // Pinpoint-Host as the acceptor host and falls back to
                    // requestAdaptor.getAcceptorHost() when the peer sent
                    // none; this endpoint is that value here. extractContext()
                    // has already stored the header if there was one, so this
                    // only fills the gap (see doc/java_parity.md).
                    impl->getSpanData()->setAcceptorHostIfAbsent(endpoint);
                    HttpTracerUtil::setProxyHeader(
                        request_reader, impl->getSpanData()->getAnnotations(),
                        impl->getConfig()->http.server.proxy_user_header_names);
                }
                span->RecordHeader(HTTP_REQUEST, request_reader);
            }
        }

        void TraceHttpServerRequest(SpanPtr span, std::string_view remote_addr, std::string_view endpoint, HeaderReader& request_reader) {
            if (!span) {
                return;
            }
            traceServerRequest(span, remote_addr, endpoint, request_reader);
        }

        void TraceHttpServerRequest(SpanPtr span, std::string_view remote_addr, std::string_view endpoint, HeaderReader& request_reader, HeaderReader& cookie_reader) {
            if (!span) {
                return;
            }
            traceServerRequest(span, remote_addr, endpoint, request_reader);
            span->RecordHeader(HTTP_COOKIE, cookie_reader);
        }

        void TraceHttpServerResponse(SpanPtr span, std::string_view url_pattern, std::string_view method, int32_t status_code, HeaderReader& response_reader){
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
            span_event->SetAnnotation(ANNOTATION_HTTP_URL, url);
            span_event->RecordHeader(HTTP_REQUEST, request_reader);
        }

        void TraceHttpClientRequest(SpanEventPtr span_event, std::string_view host, std::string_view url, HeaderReader& request_reader, HeaderReader& cookie_reader) {
            if (!span_event) {
                return;
            }
            TraceHttpClientRequest(span_event, host, url, request_reader);
            span_event->RecordHeader(HTTP_COOKIE, cookie_reader);
        }

        void TraceHttpClientResponse(SpanEventPtr span_event, int32_t status_code, HeaderReader& response_reader) {
            if (!span_event) {
                return;
            }
            span_event->SetAnnotation(ANNOTATION_HTTP_STATUS_CODE, status_code);
            span_event->RecordHeader(HTTP_RESPONSE, response_reader);
        }
    } // namespace helper
} // namespace pinpoint
