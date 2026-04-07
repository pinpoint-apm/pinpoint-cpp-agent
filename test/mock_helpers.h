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
#include <optional>
#include <string>
#include <vector>

#include "../include/pinpoint/tracer.h"

namespace pinpoint {

class MockTraceContextReader : public TraceContextReader {
public:
    MockTraceContextReader() = default;

    std::optional<std::string> Get(std::string_view key) const override {
        auto it = context_.find(std::string(key));
        if (it != context_.end()) {
            return it->second;
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

    std::optional<std::string> Get(std::string_view key) const {
        auto it = context_.find(std::string(key));
        if (it != context_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

private:
    std::map<std::string, std::string> context_;
};

class MockHeaderReader : public HeaderReader {
public:
    MockHeaderReader() = default;

    std::optional<std::string> Get(std::string_view key) const override {
        auto it = headers_.find(std::string(key));
        if (it != headers_.end()) {
            return it->second;
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
