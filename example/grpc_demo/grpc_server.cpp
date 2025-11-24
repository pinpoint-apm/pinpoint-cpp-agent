#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>

#include "pinpoint/tracer.h"

#include "pinpoint_grpc_context.h"
#include "pinpoint_grpc_interceptors.h"

#include "testapp.grpc.pb.h"

namespace grpc_demo {

class HelloServiceImpl final : public grpcdemo::Hello::Service {
 public:
  grpc::Status UnaryCallUnaryReturn(grpc::ServerContext* context,
                                    const grpcdemo::Greeting* request,
                                    grpcdemo::Greeting* response) override {
    (void)context;

    auto span = GetCurrentSpan();
    ScopedSpanEvent scoped(span, "HelloServiceImpl::UnaryCallUnaryReturn");
    if (auto event = scoped.Event()) {
      event->GetAnnotations()->AppendString(pinpoint::ANNOTATION_API,
                                            "UnaryCallUnaryReturn");
    }

    response->set_msg("Unary response: " + request->msg());
    return grpc::Status::OK;
  }

  grpc::Status UnaryCallStreamReturn(grpc::ServerContext* context,
                                     const grpcdemo::Greeting* request,
                                     grpc::ServerWriter<grpcdemo::Greeting>* writer) override {
    (void)context;

    auto span = GetCurrentSpan();
    ScopedSpanEvent scoped(span, "HelloServiceImpl::UnaryCallStreamReturn");
    if (auto event = scoped.Event()) {
      event->GetAnnotations()->AppendString(pinpoint::ANNOTATION_API,
                                            "UnaryCallStreamReturn");
    }

    for (int i = 0; i < 3; ++i) {
      grpcdemo::Greeting resp;
      resp.set_msg("Stream response " + std::to_string(i) + ": " + request->msg());
      writer->Write(resp);
    }
    return grpc::Status::OK;
  }

  grpc::Status StreamCallUnaryReturn(grpc::ServerContext* context,
                                     grpc::ServerReader<grpcdemo::Greeting>* reader,
                                     grpcdemo::Greeting* response) override {
    (void)context;

    auto span = GetCurrentSpan();
    ScopedSpanEvent scoped(span, "HelloServiceImpl::StreamCallUnaryReturn");
    if (auto event = scoped.Event()) {
      event->GetAnnotations()->AppendString(pinpoint::ANNOTATION_API,
                                            "StreamCallUnaryReturn");
    }

    std::string combined;
    grpcdemo::Greeting msg;
    while (reader->Read(&msg)) {
      combined.append(msg.msg()).append(" ");
    }
    response->set_msg("Unary response: " + combined);
    return grpc::Status::OK;
  }

  grpc::Status StreamCallStreamReturn(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<grpcdemo::Greeting, grpcdemo::Greeting>* stream) override {
    (void)context;

    auto span = GetCurrentSpan();
    ScopedSpanEvent scoped(span, "HelloServiceImpl::StreamCallStreamReturn");
    if (auto event = scoped.Event()) {
      event->GetAnnotations()->AppendString(pinpoint::ANNOTATION_API,
                                            "StreamCallStreamReturn");
    }

    grpcdemo::Greeting request;
    while (stream->Read(&request)) {
      grpcdemo::Greeting resp;
      resp.set_msg("Echo: " + request.msg());
      stream->Write(resp);
    }
    return grpc::Status::OK;
  }
};

}  // namespace grpc_demo

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  setenv("PINPOINT_CPP_APPLICATION_NAME", "grpc-server-demo", 0);
  setenv("PINPOINT_CPP_CONFIG_FILE", "/tmp/pinpoint-config.yaml", 0);
  setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "false", 0);

  auto agent = pinpoint::CreateAgent();
  if (!agent->Enable()) {
    std::cerr << "Failed to enable Pinpoint agent" << std::endl;
    return 1;
  }

  grpc_demo::HelloServiceImpl service;

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();

  grpc::ServerBuilder builder;
  builder.AddListeningPort("0.0.0.0:50051", grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>>
      interceptor_creators;
  interceptor_creators.push_back(
      std::make_unique<grpc_demo::PinpointServerInterceptorFactory>());
  builder.experimental().SetInterceptorCreators(std::move(interceptor_creators));

  auto server = builder.BuildAndStart();
  if (!server) {
    std::cerr << "Failed to start gRPC server" << std::endl;
    return 1;
  }

  std::cout << "gRPC server started on 0.0.0.0:50051" << std::endl;
  std::cout << "Methods:" << std::endl;
  std::cout << "  UnaryCallUnaryReturn" << std::endl;
  std::cout << "  UnaryCallStreamReturn" << std::endl;
  std::cout << "  StreamCallUnaryReturn" << std::endl;
  std::cout << "  StreamCallStreamReturn" << std::endl;

  server->Wait();

  agent->Shutdown();
  return 0;
}


