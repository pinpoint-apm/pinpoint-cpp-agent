package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # Apache 2

# Common header library for examples
cc_library(
    name = "example_common",
    hdrs = ["http_trace_context.h"],
    includes = ["."],
    deps = [
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
    ],
)

# Distributed tracing demo: proxy -> server -> (simulated) MySQL.
cc_binary(
    name = "distributed_proxy",
    srcs = ["proxy.cpp"],
    deps = [
        ":example_common",
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
    ],
)

cc_binary(
    name = "distributed_server",
    srcs = ["server.cpp"],
    deps = [
        ":example_common",
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
    ],
)
