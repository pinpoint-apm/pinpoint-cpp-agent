/*
 * Copyright 2020-present NAVER Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Version-neutral adapter over the public tracer API.
//
// The public surface changed incompatibly between v1.1.0 and the current
// release line, so one benchmark source cannot compile against both without a
// shim. Define PP_API_V1 to select the v1.1.0 spelling; leave it undefined for
// the current API.
//
// The shim deliberately keeps every per-call cost the *caller* pays inside the
// adapted function rather than hoisting it out. Where one version makes the
// caller do work the other does not (joining SQL bind arguments, copying a
// header value out of the carrier), that asymmetry is part of what the
// comparison is measuring and must not be optimized away on one side only.

#ifndef PINPOINT_BENCHMARK_PP_COMPAT_H
#define PINPOINT_BENCHMARK_PP_COMPAT_H

#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pinpoint/tracer.h"

namespace ppc {

#if defined(PP_API_V1) && PP_API_V1
    inline constexpr const char* kApiVariant = "v1.1.0";
    inline constexpr bool kIsV1 = true;
#else
    inline constexpr const char* kApiVariant = "main";
    inline constexpr bool kIsV1 = false;
#endif

    using AgentPtr = pinpoint::AgentPtr;
    using SpanPtr = pinpoint::SpanPtr;

    // Named the same in both versions; shared_ptr<SpanEvent> on v1.1.0 and a
    // non-owning SpanEvent* on main. Both are used through operator->.
    using EventPtr = pinpoint::SpanEventPtr;

    // ---- agent bring-up ---------------------------------------------------

    // Installs the global agent from a YAML string. Bring-up is asynchronous in
    // both versions (registration with the collector happens on a worker
    // thread), so callers must poll Enable() before creating spans that are
    // meant to be recorded.
    inline AgentPtr StartAgent(const std::string& config_yaml) {
#if defined(PP_API_V1) && PP_API_V1
        pinpoint::SetConfigString(config_yaml);
        return pinpoint::CreateAgent();
#else
        pinpoint::AgentOptions options;
        options.config_yaml = config_yaml;
        if (!pinpoint::StartAgent(options)) {
            return nullptr;
        }
        return pinpoint::GlobalAgent();
#endif
    }

    inline bool WaitUntilEnabled(const AgentPtr& agent, std::chrono::seconds timeout) {
        if (agent == nullptr) {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (agent->Enable()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return agent->Enable();
    }

    // ---- span event lifecycle ---------------------------------------------

    // v1.1.0 ends the top-of-stack event through the span; main ends it through
    // the event handle itself.
    inline void EndEvent(const SpanPtr& span, EventPtr event) {
#if defined(PP_API_V1) && PP_API_V1
        (void) event;
        span->EndSpanEvent();
#else
        (void) span;
        event->EndEvent();
#endif
    }

    // ---- annotations ------------------------------------------------------
    //
    // v1.1.0 hands out a shared_ptr<Annotation> per call site; main records
    // directly on the span/event. Separate names avoid relying on overload
    // resolution to pick the intended integer width.

    template<typename Target>
    inline void AnnotateInt(const Target& target, int32_t key, int32_t value) {
#if defined(PP_API_V1) && PP_API_V1
        target->GetAnnotations()->AppendInt(key, value);
#else
        target->SetAnnotation(key, value);
#endif
    }

    template<typename Target>
    inline void AnnotateLong(const Target& target, int32_t key, int64_t value) {
#if defined(PP_API_V1) && PP_API_V1
        target->GetAnnotations()->AppendLong(key, value);
#else
        target->SetAnnotation(key, value);
#endif
    }

    template<typename Target>
    inline void AnnotateString(const Target& target, int32_t key, std::string_view value) {
#if defined(PP_API_V1) && PP_API_V1
        target->GetAnnotations()->AppendString(key, value);
#else
        target->SetAnnotation(key, value);
#endif
    }

    template<typename Target>
    inline void AnnotateStringString(const Target& target, int32_t key,
                                     std::string_view first, std::string_view second) {
#if defined(PP_API_V1) && PP_API_V1
        target->GetAnnotations()->AppendStringString(key, first, second);
#else
        target->SetAnnotation(key, first, second);
#endif
    }

    // ---- SQL --------------------------------------------------------------

    // Both versions take the raw statement plus its bind arguments, but in
    // different shapes: v1.1.0 wants one already-joined string, main wants a
    // vector of typed values. A caller holding N discrete bind values pays the
    // conversion either way, so it stays inside the timed region.
    inline void SetSqlQuery(EventPtr event, std::string_view sql,
                            const std::vector<std::string>& bind_args) {
#if defined(PP_API_V1) && PP_API_V1
        std::string joined;
        for (size_t i = 0; i < bind_args.size(); ++i) {
            if (i != 0) {
                joined.push_back(',');
            }
            joined.append(bind_args[i]);
        }
        event->SetSqlQuery(sql, joined);
#else
        std::vector<pinpoint::SqlBindValue> values;
        values.reserve(bind_args.size());
        for (const auto& arg : bind_args) {
            values.emplace_back(std::string_view{arg});
        }
        event->SetSqlQuery(sql, values);
#endif
    }

    // ---- context propagation ----------------------------------------------

    // v1.1.0 injects the outbound context from the span; main moved it onto the
    // span event that represents the outbound call.
    inline void InjectContext(const SpanPtr& span, EventPtr event,
                              pinpoint::TraceContextWriter& writer) {
#if defined(PP_API_V1) && PP_API_V1
        (void) event;
        span->InjectContext(writer);
#else
        (void) span;
        event->InjectContext(writer);
#endif
    }

    inline std::string TraceIdString(const SpanPtr& span) {
#if defined(PP_API_V1) && PP_API_V1
        return span->GetTraceId().ToString();
#else
        return span->GetTraceId();
#endif
    }

    // ---- carriers ---------------------------------------------------------

    inline bool IEquals(std::string_view lhs, std::string_view rhs) {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (size_t i = 0; i < lhs.size(); ++i) {
            const auto a = static_cast<unsigned char>(lhs[i]);
            const auto b = static_cast<unsigned char>(rhs[i]);
            if (std::tolower(a) != std::tolower(b)) {
                return false;
            }
        }
        return true;
    }

    // Backed by a vector so lookups have the same cost shape in both builds.
    // Get() returns an owning string on v1.1.0 and a view on main: that
    // per-lookup copy is exactly one of the differences under measurement, so
    // it is left in place rather than worked around.
    class VectorHeaderReader final : public pinpoint::HeaderReader {
    public:
        explicit VectorHeaderReader(std::vector<std::pair<std::string, std::string>> headers)
            : headers_(std::move(headers)) {}

#if defined(PP_API_V1) && PP_API_V1
        std::optional<std::string> Get(std::string_view key) const override {
            for (const auto& entry : headers_) {
                if (IEquals(entry.first, key)) {
                    return entry.second;
                }
            }
            return std::nullopt;
        }
#else
        std::optional<std::string_view> Get(std::string_view key) const override {
            for (const auto& entry : headers_) {
                if (IEquals(entry.first, key)) {
                    return std::string_view{entry.second};
                }
            }
            return std::nullopt;
        }
#endif

        void ForEach(std::function<bool(std::string_view, std::string_view)> callback) const override {
            for (const auto& entry : headers_) {
                if (!callback(entry.first, entry.second)) {
                    return;
                }
            }
        }

    private:
        std::vector<std::pair<std::string, std::string>> headers_;
    };

    // Collects injected key/value pairs without allocating per call once warm.
    class VectorContextWriter final : public pinpoint::TraceContextWriter {
    public:
        void Set(std::string_view key, std::string_view value) override {
            entries_.emplace_back(key, value);
        }

        void Clear() { entries_.clear(); }
        size_t size() const { return entries_.size(); }

    private:
        std::vector<std::pair<std::string, std::string>> entries_;
    };

}  // namespace ppc

#endif  // PINPOINT_BENCHMARK_PP_COMPAT_H
