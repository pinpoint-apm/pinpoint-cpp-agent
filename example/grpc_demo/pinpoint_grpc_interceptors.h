#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <grpcpp/server_context.h>
#include <grpcpp/support/client_interceptor.h>
#include <grpcpp/support/server_interceptor.h>

#include "pinpoint/tracer.h"

#include "pinpoint_grpc_context.h"

namespace grpc_demo {

class PinpointServerInterceptor final : public grpc::experimental::Interceptor {
 public:
  explicit PinpointServerInterceptor(grpc::experimental::ServerRpcInfo* info)
      : info_(info) {}

  void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override {
    using grpc::experimental::InterceptionHookPoints;

    if (methods->QueryInterceptionHookPoint(
            InterceptionHookPoints::POST_RECV_INITIAL_METADATA)) {
      if (!span_) {
        auto* server_context_base = info_->server_context();
        GrpcServerTraceContextReader reader(server_context_base);
        auto agent = pinpoint::GlobalAgent();
        const std::string rpc_method = info_->method();
        span_ = agent->NewSpan("gRPC Server", rpc_method, reader);

        if (span_ && span_->IsSampled()) {
          span_->SetServiceType(pinpoint::SERVICE_TYPE_GRPC_SERVER);
          if (server_context_base != nullptr) {
            span_->SetRemoteAddress(server_context_base->peer());
          }
          span_->GetAnnotations()->AppendString(pinpoint::ANNOTATION_API, rpc_method);
        }

        SetCurrentSpan(span_);
      }
    }

    if (methods->QueryInterceptionHookPoint(
            InterceptionHookPoints::PRE_SEND_STATUS)) {
      if (span_) {
        grpc::Status status = methods->GetSendStatus();
        span_->SetStatusCode(static_cast<int>(status.error_code()));
        if (!status.ok()) {
          span_->SetError(status.error_message());
        }
        span_->EndSpan();
        span_.reset();
      }
      ClearCurrentSpan();
    }

    methods->Proceed();
  }

 private:
  grpc::experimental::ServerRpcInfo* info_;
  pinpoint::SpanPtr span_;
};

class PinpointServerInterceptorFactory final
    : public grpc::experimental::ServerInterceptorFactoryInterface {
 public:
  grpc::experimental::Interceptor* CreateServerInterceptor(
      grpc::experimental::ServerRpcInfo* info) override {
    return new PinpointServerInterceptor(info);
  }
};

class PinpointClientInterceptor final : public grpc::experimental::Interceptor {
 public:
  explicit PinpointClientInterceptor(grpc::experimental::ClientRpcInfo* info)
      : info_(info) {}

  void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override {
    using grpc::experimental::InterceptionHookPoints;
    if (methods->QueryInterceptionHookPoint(
            InterceptionHookPoints::PRE_SEND_INITIAL_METADATA)) {
      parent_span_ = GetCurrentSpan();
      auto agent = pinpoint::GlobalAgent();
      const std::string rpc_method = info_->method();

      if (parent_span_) {
        span_event_ = parent_span_->NewSpanEvent(rpc_method);
        if (span_event_) {
          span_event_->SetServiceType(pinpoint::SERVICE_TYPE_GRPC_CLIENT);
          if (info_->channel()) {
            span_event_->SetDestination(info_->client_context()->peer());
          }
        }

        if (auto* context = info_->client_context()) {
          GrpcClientTraceContextWriter writer(context);
          parent_span_->InjectContext(writer);
        }
        parent_span_->GetAnnotations()->AppendString(pinpoint::ANNOTATION_API, rpc_method);
      } else {
        call_span_ = agent->NewSpan("gRPC Client", rpc_method);
        if (call_span_ && call_span_->IsSampled()) {
          call_span_->SetServiceType(pinpoint::SERVICE_TYPE_GRPC_CLIENT);
          if (info_->channel()) {
            call_span_->SetRemoteAddress(info_->client_context()->peer());
          }
          call_span_->GetAnnotations()->AppendString(pinpoint::ANNOTATION_API, rpc_method);
        }

        if (auto* context = info_->client_context()) {
          GrpcClientTraceContextWriter writer(context);
          call_span_->InjectContext(writer);
        }
        SetCurrentSpan(call_span_);
      }
    }

    if (methods->QueryInterceptionHookPoint(
            InterceptionHookPoints::POST_RECV_STATUS)) {
      if (span_event_ && parent_span_) {
        if (auto* status = methods->GetRecvStatus(); status != nullptr && !status->ok()) {
          span_event_->SetError(status->error_message());
        }
        parent_span_->EndSpanEvent();
        span_event_.reset();
      }

      if (call_span_) {
        if (auto* status = methods->GetRecvStatus()) {
          call_span_->SetStatusCode(static_cast<int>(status->error_code()));
          if (!status->ok()) {
            call_span_->SetError(status->error_message());
          }
        }
        call_span_->EndSpan();
        call_span_.reset();
        ClearCurrentSpan();
      }
    }

    methods->Proceed();
  }

 private:
  grpc::experimental::ClientRpcInfo* info_;
  pinpoint::SpanPtr parent_span_;
  pinpoint::SpanPtr call_span_;
  pinpoint::SpanEventPtr span_event_;
};

class PinpointClientInterceptorFactory final
    : public grpc::experimental::ClientInterceptorFactoryInterface {
 public:
  grpc::experimental::Interceptor* CreateClientInterceptor(
      grpc::experimental::ClientRpcInfo* info) override {
    return new PinpointClientInterceptor(info);
  }
};

}  // namespace grpc_demo


