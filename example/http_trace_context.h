#include <algorithm>
#include <string_view>
#include "3rd_party/httplib.h"

#include "pinpoint/tracer.h"

class HttpTraceContextReader final : public pinpoint::TraceContextReader {
public:
    explicit HttpTraceContextReader(const httplib::Headers& header) : headers_(header) {}
    ~HttpTraceContextReader() override = default;

    std::optional<std::string> Get(std::string_view key) const override {
        auto it = headers_.find(key.data());
        if (it == headers_.end()) {
            return std::nullopt;
        }
        return it->second;
    }
private:
    const httplib::Headers& headers_;
};

class HttpTraceContextWriter final : public pinpoint::TraceContextWriter {
public:
    explicit HttpTraceContextWriter(httplib::Headers& header) : headers_(header) {}
    ~HttpTraceContextWriter() override = default;

    void Set(std::string_view key, std::string_view value) override {
        headers_.emplace(key, value);
    }
private:
    httplib::Headers& headers_;
};

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
