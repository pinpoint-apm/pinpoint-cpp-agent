#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_context.h>

#include "pinpoint/tracer.h"

namespace grpc_demo {

// Thread-local storage for the active span within the current thread.
inline thread_local pinpoint::SpanPtr g_current_span = nullptr;

inline void SetCurrentSpan(const pinpoint::SpanPtr& span) { g_current_span = span; }

inline pinpoint::SpanPtr GetCurrentSpan() { return g_current_span; }

inline void ClearCurrentSpan() { g_current_span.reset(); }

// Utility class to read Pinpoint trace context from gRPC server metadata.
class GrpcServerTraceContextReader final : public pinpoint::TraceContextReader {
 public:
  explicit GrpcServerTraceContextReader(grpc::ServerContextBase* context)
      : context_(context) {}
  ~GrpcServerTraceContextReader() override = default;

  std::optional<std::string> Get(std::string_view key) const override {
    if (context_ == nullptr) {
      return std::nullopt;
    }

    const auto& metadata = context_->client_metadata();
    for (const auto& entry : metadata) {
      std::string entry_key(entry.first.data(), entry.first.size());
      if (entry_key == key) {
        return std::string(entry.second.data(), entry.second.size());
      }
    }
    return std::nullopt;
  }

 private:
  grpc::ServerContextBase* context_;
};

// Utility class to inject Pinpoint trace context into gRPC client metadata.
class GrpcClientTraceContextWriter final : public pinpoint::TraceContextWriter {
 public:
  explicit GrpcClientTraceContextWriter(grpc::ClientContext* context) : context_(context) {}
  ~GrpcClientTraceContextWriter() override = default;

  void Set(std::string_view key, std::string_view value) override {
    if (context_ == nullptr) {
      return;
    }

    context_->AddMetadata(std::string(key), std::string(value));
  }

 private:
  grpc::ClientContext* context_;
};

}  // namespace grpc_demo


