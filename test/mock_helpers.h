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

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../include/pinpoint/tracer.h"
#include "../src/span.h"

namespace pinpoint {

inline SpanData make_test_span_data(AgentService& agent, std::string_view operation) {
    return SpanData(operation, agent.getAppType(), agent.cacheApi(operation, API_TYPE_WEB_REQUEST));
}

inline std::shared_ptr<SpanData> make_test_span_data_ptr(AgentService& agent, std::string_view operation) {
    return std::make_shared<SpanData>(operation, agent.getAppType(), agent.cacheApi(operation, API_TYPE_WEB_REQUEST));
}

inline SpanEventImpl make_test_span_event(SpanImpl& span, std::string_view operation) {
    return SpanEventImpl(&span, operation);
}

inline std::unique_ptr<SpanEventImpl> make_test_span_event_unique(SpanImpl& span, std::string_view operation) {
    return std::make_unique<SpanEventImpl>(&span, operation);
}

class MockTraceContextReader : public TraceContextReader {
public:
    MockTraceContextReader() = default;

    std::optional<std::string_view> Get(std::string_view key) const override {
        auto it = context_.find(std::string(key));
        if (it != context_.end()) {
            return std::string_view(it->second);
        }
        return std::nullopt;
    }

    void SetContext(std::string_view key, std::string_view value) {
        context_[std::string(key)] = std::string(value);
    }

private:
    std::map<std::string, std::string> context_;
};

class MockTraceContextWriter : public TraceContextWriter {
public:
    MockTraceContextWriter() = default;

    void Set(std::string_view key, std::string_view value) override {
        context_[std::string(key)] = std::string(value);
    }

    std::optional<std::string_view> Get(std::string_view key) const {
        auto it = context_.find(std::string(key));
        if (it != context_.end()) {
            return std::string_view(it->second);
        }
        return std::nullopt;
    }

private:
    std::map<std::string, std::string> context_;
};

class MockHeaderReader : public HeaderReader {
public:
    MockHeaderReader() = default;

    std::optional<std::string_view> Get(std::string_view key) const override {
        auto it = headers_.find(std::string(key));
        if (it != headers_.end()) {
            return std::string_view(it->second);
        }
        return std::nullopt;
    }

    void ForEach(std::function<bool(std::string_view key, std::string_view val)> callback) const override {
        for (const auto& pair : headers_) {
            if (!callback(pair.first, pair.second)) {
                break;
            }
        }
    }

    void SetHeader(std::string_view key, std::string_view value) {
        headers_[std::string(key)] = std::string(value);
    }

private:
    std::map<std::string, std::string> headers_;
};

class MockCallStackReader : public CallStackReader {
public:
    MockCallStackReader() = default;

    void ForEach(std::function<void(std::string_view module, std::string_view function, std::string_view file, int line)> callback) const override {
        for (const auto& frame : frames_) {
            callback(frame.module, frame.function, frame.file, frame.line);
        }
    }

    void AddFrame(std::string_view module, std::string_view function, std::string_view file, int line) {
        frames_.push_back({std::string(module), std::string(function), std::string(file), line});
    }

    size_t GetFrameCount() const { return frames_.size(); }

private:
    struct Frame {
        std::string module;
        std::string function;
        std::string file;
        int line;
    };
    std::vector<Frame> frames_;
};

}  // namespace pinpoint
