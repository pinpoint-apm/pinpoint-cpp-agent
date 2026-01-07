cc_binary(
    name = "tutorial",
    srcs = [
        "http_trace_context.h",
        "tutorial.cpp",
    ],
    tags = ["examples"],
    deps = [
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
    ],
)

cc_binary(
    name = "http_server",
    srcs = [
        "http_server.cpp",
        "http_trace_context.h",
    ],
    tags = ["examples"],
    deps = [
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
    ],
)
