#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "pinpoint/tracer.h"

#include "pinpoint_grpc_context.h"
#include "pinpoint_grpc_interceptors.h"

#include "testapp.grpc.pb.h"

namespace grpc_demo {

std::unique_ptr<grpcdemo::Hello::Stub> CreateStubWithInterceptor(
    const std::string& target) {
  grpc::ChannelArguments args;
  std::vector<std::unique_ptr<grpc::experimental::ClientInterceptorFactoryInterface>>
      interceptor_creators;
  interceptor_creators.push_back(
      std::make_unique<PinpointClientInterceptorFactory>());

  auto channel = grpc::experimental::CreateCustomChannelWithInterceptors(
      target, grpc::InsecureChannelCredentials(), args,
      std::move(interceptor_creators));
  return grpcdemo::Hello::NewStub(channel);
}

void CallUnary(grpcdemo::Hello::Stub* stub) {
  auto agent = pinpoint::GlobalAgent();
  auto span = agent->NewSpan("gRPC Client Unary", "/grpcdemo.Hello/UnaryCallUnaryReturn");
  SetCurrentSpan(span);

  grpc::ClientContext context;
  grpcdemo::Greeting request;
  grpcdemo::Greeting response;
  request.set_msg("Hello from unary client");

  auto status = stub->UnaryCallUnaryReturn(&context, request, &response);
  if (!status.ok()) {
    span->SetError(status.error_message());
  } else {
    span->GetAnnotations()->AppendString(pinpoint::ANNOTATION_API, response.msg());
  }

  span->EndSpan();
  ClearCurrentSpan();
}

void CallServerStreaming(grpcdemo::Hello::Stub* stub) {
  auto agent = pinpoint::GlobalAgent();
  auto span = agent->NewSpan("gRPC Client ServerStream",
                             "/grpcdemo.Hello/UnaryCallStreamReturn");
  SetCurrentSpan(span);

  grpc::ClientContext context;
  grpcdemo::Greeting request;
  request.set_msg("Stream greetings");

  auto reader = stub->UnaryCallStreamReturn(&context, request);
  grpcdemo::Greeting response;
  while (reader->Read(&response)) {
    std::cout << "Server stream: " << response.msg() << std::endl;
  }
  auto status = reader->Finish();
  if (!status.ok()) {
    span->SetError(status.error_message());
  }

  span->EndSpan();
  ClearCurrentSpan();
}

void CallBidirectional(grpcdemo::Hello::Stub* stub) {
  auto agent = pinpoint::GlobalAgent();
  auto span = agent->NewSpan("gRPC Client Bidi",
                             "/grpcdemo.Hello/StreamCallStreamReturn");
  SetCurrentSpan(span);

  grpc::ClientContext context;
  auto stream = stub->StreamCallStreamReturn(&context);

  for (int i = 0; i < 3; ++i) {
    grpcdemo::Greeting request;
    request.set_msg("Message " + std::to_string(i));
    stream->Write(request);
    grpcdemo::Greeting response;
    if (stream->Read(&response)) {
      std::cout << "Bidi response: " << response.msg() << std::endl;
    }
  }
  stream->WritesDone();
  auto status = stream->Finish();
  if (!status.ok()) {
    span->SetError(status.error_message());
  }

  span->EndSpan();
  ClearCurrentSpan();
}

}  // namespace grpc_demo

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  setenv("PINPOINT_CPP_APPLICATION_NAME", "grpc-client-demo", 0);
  setenv("PINPOINT_CPP_CONFIG_FILE", "/tmp/pinpoint-config.yaml", 0);

  auto agent = pinpoint::CreateAgent();
  if (!agent->Enable()) {
    std::cerr << "Failed to enable Pinpoint agent" << std::endl;
    return 1;
  }

  auto stub = grpc_demo::CreateStubWithInterceptor("localhost:50051");

  grpc_demo::CallUnary(stub.get());
  grpc_demo::CallServerStreaming(stub.get());
  grpc_demo::CallBidirectional(stub.get());

  agent->Shutdown();
  return 0;
}


