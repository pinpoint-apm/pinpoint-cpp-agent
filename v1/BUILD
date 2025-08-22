load("@rules_proto//proto:defs.bzl", "proto_library")
load("@rules_cc//cc:defs.bzl", "cc_proto_library")
load("@com_github_grpc_grpc//bazel:cc_grpc_library.bzl", "cc_grpc_library")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # Apache 2

proto_library(
    name = "annotation_proto",
    srcs = ["Annotation.proto"],
    deps = ["@com_google_protobuf//:wrappers_proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "annotation_proto_cc",
    deps = [":annotation_proto"],
)

cc_grpc_library(
    name = "annotation_grpc_cc",
    srcs = [":annotation_proto"],
    generate_mocks = True,
    grpc_only = True,
    deps = [":annotation_proto_cc"],
)

proto_library(
    name = "thread_dump_proto",
    srcs = ["ThreadDump.proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "thread_dump_proto_cc",
    deps = [":thread_dump_proto"],
)

cc_grpc_library(
    name = "thread_dump_grpc_cc",
    srcs = [":thread_dump_proto"],
    generate_mocks = True,
    grpc_only = True,
    deps = [":thread_dump_proto_cc"],
)

proto_library(
    name = "cmd_proto",
    srcs = ["Cmd.proto"],
    deps = [
        "@com_google_protobuf//:wrappers_proto",
        ":thread_dump_proto"
    ],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "cmd_proto_cc",
    deps = [":cmd_proto"],
)

cc_grpc_library(
    name = "cmd_grpc_cc",
    srcs = [":cmd_proto"],
    generate_mocks = True,
    grpc_only = True,
    deps = [":cmd_proto_cc"],
)

proto_library(
    name = "span_proto",
    srcs = ["Span.proto"],
    deps = [":annotation_proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "span_proto_cc",
    deps = [":span_proto"],
)

cc_grpc_library(
    name = "span_grpc_cc",
    srcs = [":span_proto"],
    generate_mocks = True,
    grpc_only = True,
    deps = [":span_proto_cc"],
)

proto_library(
    name = "stat_proto",
    srcs = ["Stat.proto"],
    deps = [":thread_dump_proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "stat_proto_cc",
    deps = [":stat_proto"],
)

cc_grpc_library(
    name = "stat_grpc_cc",
    srcs = [":stat_proto"],
    generate_mocks = True,
    grpc_only = True,
    deps = [":stat_proto_cc"],
)

proto_library(
    name = "service_proto",
    srcs = ["Service.proto"],
    deps = [
        "@com_google_protobuf//:empty_proto",
        ":cmd_proto",
        ":span_proto",
        ":stat_proto",
    ],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "service_proto_cc",
    deps = [":service_proto"],
)

cc_grpc_library(
    name = "service_grpc_cc",
    srcs = [":service_proto"],
    generate_mocks = True,
    grpc_only = True,
    deps = [":service_proto_cc"],
)
