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

# Pure-C wrapper around cpp-httplib used by the C-only examples.
# Built as C++ internally; exposes a pure-C header.
cc_library(
    name = "httplib_c",
    srcs = ["httplib_c.cpp"],
    hdrs = ["httplib_c.h"],
    includes = ["."],
    deps = [
        "//3rd_party:httplib",
    ],
)

# HTTP Server example (C port)
cc_binary(
    name = "http_server_c",
    srcs = ["http_server_c.c"],
    tags = ["examples"],
    deps = [
        ":httplib_c",
        "//:pinpoint-cpp",
    ],
)

# Tutorial example (C port)
cc_binary(
    name = "tutorial_c",
    srcs = ["tutorial_c.c"],
    tags = ["examples"],
    deps = [
        ":httplib_c",
        "//:pinpoint-cpp",
    ],
)

# Example suite (buildable without external dependencies)
filegroup(
    name = "all_examples",
    srcs = [
        ":http_server",
        ":http_server_c",
        ":tutorial",
        ":tutorial_c",
    ],
)
