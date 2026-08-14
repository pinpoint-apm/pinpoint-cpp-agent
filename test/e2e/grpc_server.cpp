#include <iostream>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
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
    scoped->SetAnnotation(pinpoint::ANNOTATION_API, "UnaryCallUnaryReturn");

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
    scoped->SetAnnotation(pinpoint::ANNOTATION_API, "UnaryCallStreamReturn");

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
    scoped->SetAnnotation(pinpoint::ANNOTATION_API, "StreamCallUnaryReturn");

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
    scoped->SetAnnotation(pinpoint::ANNOTATION_API, "StreamCallStreamReturn");

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

namespace {

volatile std::sig_atomic_t g_stop = 0;

void request_stop(int) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
  // Redirected stdout is block-buffered, and the process then blocks serving
  // requests until it is killed, so run_e2e.sh's log greps can race the
  // unflushed tail. Matches e2e_server.cpp.
  setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);

  int port = 50051;
  if (argc > 1) {
    port = std::atoi(argv[1]);
  }

  it_test::ConfigureAgentEnvironment("cpp-it-grpc-downstream",
                                     "cpp-it-grpc-down");
  setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "false", 0);

  pinpoint::AgentOptions agent_options;
  agent_options.app_type = pinpoint::APP_TYPE_CPP;
  agent_options.server_info = "cpp-it-grpc-downstream";
  agent_options.args = {"--port=" + std::to_string(port)};
  agent_options.libs = {"grpc"};
  if (!pinpoint::StartAgent(agent_options)) {
    std::cerr << "pinpoint agent start failed; check the agent log" << std::endl;
  }
  auto agent = pinpoint::GlobalAgent();

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

  // This server has no control channel, so run_e2e.sh stops it with SIGTERM.
  // The default action terminates the process outright and skips the exit
  // handlers — including the one that writes the coverage profile, which is
  // why this server contributed nothing to a coverage run. Catch the signal
  // and leave through gRPC's own shutdown instead. The handler only sets a
  // flag: Shutdown() is not async-signal-safe, and the signal interrupts the
  // sleep below so the wait stays short.
  std::signal(SIGTERM, request_stop);
  std::signal(SIGINT, request_stop);
  while (g_stop == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  server->Shutdown();
  server->Wait();

  agent->Shutdown();
  return 0;
}
