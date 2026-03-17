package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # Apache 2

# Common header library for examples
cc_library(
    name = "example_common",
    hdrs = ["http_trace_context.h"],
    deps = [
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
    ],
)

# Tutorial example
cc_binary(
    name = "tutorial",
    srcs = ["tutorial.cpp"],
    tags = ["examples"],
    deps = [
        ":example_common",
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
    ],
)

# HTTP Server example
cc_binary(
    name = "http_server",
    srcs = ["http_server.cpp"],
    tags = ["examples"],
    deps = [
        ":example_common",
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
    ],
)

# Example suite (buildable without external dependencies)
filegroup(
    name = "all_examples",
    srcs = [
        ":http_server",
        ":tutorial",
    ],
)
