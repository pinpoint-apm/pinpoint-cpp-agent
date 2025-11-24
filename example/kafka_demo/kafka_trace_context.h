#ifndef KAFKA_TRACE_CONTEXT_H
#define KAFKA_TRACE_CONTEXT_H

#include "pinpoint/tracer.h"
#include <librdkafka/rdkafkacpp.h>
#include <string>
#include <optional>

class KafkaTraceContextReader : public pinpoint::TraceContextReader {
public:
    explicit KafkaTraceContextReader(const RdKafka::Headers* headers) : headers_(headers) {}

    std::optional<std::string> Get(std::string_view key) const override {
        if (!headers_) {
            return std::nullopt;
        }

        std::string key_str(key);
        auto result = headers_->get_last(key_str);
        
        if (result.err() == RdKafka::ErrorCode::ERR_NO_ERROR) {
            return std::string(result.value_string());
        }
        return std::nullopt;
    }

private:
    const RdKafka::Headers* headers_;
};

class KafkaTraceContextWriter : public pinpoint::TraceContextWriter {
public:
    explicit KafkaTraceContextWriter(RdKafka::Headers* headers) : headers_(headers) {}

    void Set(std::string_view key, std::string_view value) override {
        if (!headers_) return;
        headers_->add(std::string(key), std::string(value));
    }

private:
    RdKafka::Headers* headers_;
};

#endif // KAFKA_TRACE_CONTEXT_H
