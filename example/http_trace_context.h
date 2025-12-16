#include <algorithm>
#include <string_view>

#include "httplib.h"
#include "pinpoint/tracer.h"

class HttpHeaderReader final : public pinpoint::HeaderReader {
public:
    explicit HttpHeaderReader(const httplib::Headers& header) : headers_(header) {}
    ~HttpHeaderReader() override = default;

    std::optional<std::string> Get(std::string_view key) const override {
        auto it = headers_.find(key.data());
        if (it == headers_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void ForEach(std::function<bool(std::string_view, std::string_view)> callback) const override {
        std::for_each(headers_.begin(), headers_.end(), [callback](const auto& pair) {
          callback(pair.first, pair.second);
        });
    }

private:
    const httplib::Headers& headers_;
};

class HttpHeaderReaderWriter final : public pinpoint::HeaderReaderWriter {
public:
    explicit HttpHeaderReaderWriter(httplib::Headers& header) : headers_(header) {}
    ~HttpHeaderReaderWriter() override = default;

    std::optional<std::string> Get(std::string_view key) const override {
        auto it = headers_.find(key.data());
        if (it == headers_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void ForEach(std::function<bool(std::string_view, std::string_view)> callback) const override {
        std::for_each(headers_.begin(), headers_.end(), [callback](const auto& pair) {
          callback(pair.first, pair.second);
        });
    }

    void Set(std::string_view key, std::string_view value) override {
        headers_.emplace(key, value);
    }

private:
    httplib::Headers& headers_;
};
