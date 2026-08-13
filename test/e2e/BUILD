load("@rules_proto//proto:defs.bzl", "proto_library")
load("@rules_cc//cc:defs.bzl", "cc_proto_library")
load("@rules_shell//shell:sh_binary.bzl", "sh_binary")
load("@rules_shell//shell:sh_test.bzl", "sh_test")
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
    "-Itest/e2e",
    "-I$(GENDIR)/test/e2e",
]

# gRPC server (standalone)
cc_binary(
    name = "grpc_server",
    srcs = [
        "grpc_server.cpp",
        "e2e_common.h",
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

# HTTP downstream server used to verify real HTTP context propagation.
cc_binary(
    name = "http_downstream_server",
    srcs = [
        "http_downstream_server.cpp",
        "e2e_common.h",
    ],
    deps = [
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
        "//example:example_common",
    ],
)

# it_test_server (HTTP + gRPC client + SQL tracing)
cc_binary(
    name = "it_test_server",
    srcs = [
        "e2e_server.cpp",
        "e2e_common.h",
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

# Pure-C public API and cold-create/fork lifecycle scenarios.
cc_binary(
    name = "c_api_scenario",
    srcs = ["c_api_scenario.c"],
    linkopts = ["-pthread"],
    deps = ["//:pinpoint-cpp"],
)

cc_binary(
    name = "fork_scenario",
    srcs = [
        "fork_scenario.cpp",
        "e2e_common.h",
    ],
    linkopts = ["-pthread"],
    deps = ["//:pinpoint-cpp"],
)

_LIVE_IT_DATA = [
    ":c_api_scenario",
    ":fork_scenario",
    ":grpc_server",
    ":http_downstream_server",
    ":it_test_server",
    "load_test.py",
    "pinpoint-config.yaml",
    "profile_load.sh",
    "smoke_test.sh",
]

sh_binary(
    name = "run_it_test",
    srcs = ["run_e2e.sh"],
    data = _LIVE_IT_DATA,
)

sh_test(
    name = "live_integration_test",
    srcs = ["run_e2e.sh"],
    data = _LIVE_IT_DATA,
    tags = [
        "exclusive",
        "manual",
        "requires-network",
    ],
)
