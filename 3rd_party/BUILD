load("@rules_proto//proto:defs.bzl", "proto_library")
load("@rules_cc//cc:defs.bzl", "cc_proto_library")
load("@com_github_grpc_grpc//bazel:cc_grpc_library.bzl", "cc_grpc_library")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # Apache 2

cc_library(
    name = "murmur_hash3",
    srcs = [
        "MurmurHash3.cpp",
        "MurmurHash3.h",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "httplib",
    hdrs = [
        "httplib.h",
    ],
    includes = ["."],
    visibility = ["//visibility:public"],
)

# Pinpoint gRPC IDL — vendored via git submodule at pinpoint-grpc-idl/.
# The submodule ships no BUILD file, so its files are part of //3rd_party.
# strip_import_prefix trims the on-disk prefix so `import "v1/X.proto";`
# inside the IDL resolves, and generated headers land at v1/X.pb.h.

_IDL_STRIP_PREFIX = "pinpoint-grpc-idl/proto"

proto_library(
    name = "annotation_proto",
    srcs = ["pinpoint-grpc-idl/proto/v1/Annotation.proto"],
    strip_import_prefix = _IDL_STRIP_PREFIX,
    deps = ["@com_google_protobuf//:wrappers_proto"],
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
    srcs = ["pinpoint-grpc-idl/proto/v1/ThreadDump.proto"],
    strip_import_prefix = _IDL_STRIP_PREFIX,
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
    srcs = ["pinpoint-grpc-idl/proto/v1/Cmd.proto"],
    strip_import_prefix = _IDL_STRIP_PREFIX,
    deps = [
        ":thread_dump_proto",
        "@com_google_protobuf//:wrappers_proto",
    ],
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
    srcs = ["pinpoint-grpc-idl/proto/v1/Span.proto"],
    strip_import_prefix = _IDL_STRIP_PREFIX,
    deps = [":annotation_proto"],
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
    srcs = ["pinpoint-grpc-idl/proto/v1/Stat.proto"],
    strip_import_prefix = _IDL_STRIP_PREFIX,
    deps = [":thread_dump_proto"],
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
    srcs = ["pinpoint-grpc-idl/proto/v1/Service.proto"],
    strip_import_prefix = _IDL_STRIP_PREFIX,
    deps = [
        ":cmd_proto",
        ":span_proto",
        ":stat_proto",
        "@com_google_protobuf//:empty_proto",
    ],
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
