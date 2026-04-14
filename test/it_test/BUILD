load("@rules_proto//proto:defs.bzl", "proto_library")
load("@rules_cc//cc:defs.bzl", "cc_proto_library")
load("@com_github_grpc_grpc//bazel:cc_grpc_library.bzl", "cc_grpc_library")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # Apache 2

proto_library(
    name = "testapp_proto",
    srcs = ["testapp.proto"],
)

cc_proto_library(
    name = "testapp_proto_cc",
    deps = [":testapp_proto"],
)

cc_grpc_library(
    name = "testapp_grpc_cc",
    srcs = [":testapp_proto"],
    grpc_only = True,
    deps = [":testapp_proto_cc"],
)

_PROTO_COPTS = [
    "-Itest/it_test",
    "-I$(GENDIR)/test/it_test",
]

# gRPC server (standalone)
cc_binary(
    name = "grpc_server",
    srcs = [
        "grpc_server.cpp",
        "pinpoint_grpc_context.h",
        "pinpoint_grpc_interceptors.h",
    ],
    copts = _PROTO_COPTS,
    deps = [
        ":testapp_grpc_cc",
        "//:pinpoint-cpp",
        "@com_github_grpc_grpc//:grpc++",
        "@com_github_grpc_grpc//:grpc++_reflection",
    ],
)

# it_test_server (HTTP + gRPC client + SQL tracing)
cc_binary(
    name = "it_test_server",
    srcs = [
        "it_test_server.cpp",
        "pinpoint_grpc_context.h",
        "pinpoint_grpc_interceptors.h",
    ],
    copts = _PROTO_COPTS,
    deps = [
        ":testapp_grpc_cc",
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
        "//example:example_common",
        "@com_github_grpc_grpc//:grpc++",
    ],
)
