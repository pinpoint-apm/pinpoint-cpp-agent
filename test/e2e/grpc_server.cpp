#include <iostream>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>

#include "pinpoint/tracer.h"

#include "e2e_common.h"
#include "pinpoint_grpc_context.h"
#include "pinpoint_grpc_interceptors.h"

#include "testapp.grpc.pb.h"

namespace grpc_demo {

void FillTraceMetadata(grpcdemo::Greeting* response) {
  auto span = GetCurrentSpan();
  if (!span || !response) {
    return;
  }
  response->set_trace_id(span->GetTraceId());
  response->set_span_id(span->GetSpanId());
  response->set_sampled(span->IsSampled());
}

class HelloServiceImpl final : public grpcdemo::Hello::Service {
 public:
  grpc::Status UnaryCallUnaryReturn(grpc::ServerContext* context,
                                    const grpcdemo::Greeting* request,
                                    grpcdemo::Greeting* response) override {
    (void)context;

    auto span = GetCurrentSpan();
    pinpoint::helper::ScopedSpanEvent scoped(span, "HelloServiceImpl::UnaryCallUnaryReturn");
    scoped->GetAnnotations()->AppendString(pinpoint::ANNOTATION_API,
                                            "UnaryCallUnaryReturn");

    if (request->msg() == "force-error") {
      scoped->SetError("ForcedGrpcError", "forced integration-test error");
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "forced integration-test error");
    }

    response->set_msg("Unary response: " + request->msg());
    FillTraceMetadata(response);
    return grpc::Status::OK;
  }

  grpc::Status UnaryCallStreamReturn(grpc::ServerContext* context,
                                     const grpcdemo::Greeting* request,
                                     grpc::ServerWriter<grpcdemo::Greeting>* writer) override {
    (void)context;

    auto span = GetCurrentSpan();
    pinpoint::helper::ScopedSpanEvent scoped(span, "HelloServiceImpl::UnaryCallStreamReturn");
    scoped->GetAnnotations()->AppendString(pinpoint::ANNOTATION_API,
                                            "UnaryCallStreamReturn");

    for (int i = 0; i < 3; ++i) {
      grpcdemo::Greeting resp;
      resp.set_msg("Stream response " + std::to_string(i) + ": " + request->msg());
      FillTraceMetadata(&resp);
      writer->Write(resp);
    }
    return grpc::Status::OK;
  }

  grpc::Status StreamCallUnaryReturn(grpc::ServerContext* context,
                                     grpc::ServerReader<grpcdemo::Greeting>* reader,
                                     grpcdemo::Greeting* response) override {
    (void)context;

    auto span = GetCurrentSpan();
    pinpoint::helper::ScopedSpanEvent scoped(span, "HelloServiceImpl::StreamCallUnaryReturn");
    scoped->GetAnnotations()->AppendString(pinpoint::ANNOTATION_API,
                                            "StreamCallUnaryReturn");

    std::string combined;
    grpcdemo::Greeting msg;
    while (reader->Read(&msg)) {
      combined.append(msg.msg()).append(" ");
    }
    response->set_msg("Unary response: " + combined);
    FillTraceMetadata(response);
    return grpc::Status::OK;
  }

  grpc::Status StreamCallStreamReturn(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<grpcdemo::Greeting, grpcdemo::Greeting>* stream) override {
    (void)context;

    auto span = GetCurrentSpan();
    pinpoint::helper::ScopedSpanEvent scoped(span, "HelloServiceImpl::StreamCallStreamReturn");
    scoped->GetAnnotations()->AppendString(pinpoint::ANNOTATION_API,
                                            "StreamCallStreamReturn");

    grpcdemo::Greeting request;
    while (stream->Read(&request)) {
      grpcdemo::Greeting resp;
      resp.set_msg("Echo: " + request.msg());
      FillTraceMetadata(&resp);
      stream->Write(resp);
    }
    return grpc::Status::OK;
  }
};

}  // namespace grpc_demo

int main(int argc, char** argv) {
  int port = 50051;
  if (argc > 1) {
    port = std::atoi(argv[1]);
  }

  it_test::ConfigureAgentEnvironment("cpp-it-grpc-downstream",
                                     "cpp-it-grpc-down");
  setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "false", 0);

  auto agent = pinpoint::CreateAgent(
      pinpoint::APP_TYPE_CPP, "cpp-it-grpc-downstream",
      {"--port=" + std::to_string(port)}, {"grpc"});
  // CreateAgent() is cold; Start() brings the agent online in this process.
  agent->Start();

  grpc_demo::HelloServiceImpl service;

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();

  grpc::ServerBuilder builder;
  std::string listen_addr = "0.0.0.0:" + std::to_string(port);
  builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
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

  std::cout << "gRPC server started on " << listen_addr
            << " (collector=" << it_test::CollectorHost() << ")" << std::endl;
  std::cout << "Methods:" << std::endl;
  std::cout << "  UnaryCallUnaryReturn" << std::endl;
  std::cout << "  UnaryCallStreamReturn" << std::endl;
  std::cout << "  StreamCallUnaryReturn" << std::endl;
  std::cout << "  StreamCallStreamReturn" << std::endl;

  server->Wait();

  agent->Shutdown();
  return 0;
}
