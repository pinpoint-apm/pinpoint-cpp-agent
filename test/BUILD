package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # Apache 2

# Test library with common dependencies
cc_library(
    name = "test_common",
    testonly = True,
    includes = [
        "../src",
    ],
    deps = [
        "//:pinpoint-cpp",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

# Test library for gmock tests
cc_library(
    name = "test_gmock",
    testonly = True,
    includes = [
        "../src",
    ],
    deps = [
        "//:pinpoint-cpp",
        "@googletest//:gtest_main",
    ],
)

# Rate Limiter tests
cc_test(
    name = "test_limiter",
    size = "small",
    srcs = ["test_limiter.cpp"],
    deps = [":test_common"],
)

# Sampling tests
cc_test(
    name = "test_sampling",
    size = "small",
    srcs = ["test_sampling.cpp"],
    deps = [":test_common"],
)

# Cache tests
cc_test(
    name = "test_cache",
    size = "small",
    srcs = ["test_cache.cpp"],
    deps = [":test_common"],
)

# HTTP tests
cc_test(
    name = "test_http",
    size = "small",
    srcs = ["test_http.cpp"],
    deps = [":test_common"],
)

# Annotation tests
cc_test(
    name = "test_annotation",
    size = "small",
    srcs = ["test_annotation.cpp"],
    deps = [":test_common"],
)

# CallStack tests
cc_test(
    name = "test_callstack",
    size = "small",
    srcs = ["test_callstack.cpp"],
    deps = [":test_common"],
)

# Config tests
cc_test(
    name = "test_config",
    size = "small",
    srcs = ["test_config.cpp"],
    deps = [":test_common"],
)

# SQL tests
cc_test(
    name = "test_sql",
    size = "small",
    srcs = ["test_sql.cpp"],
    deps = [":test_common"],
)

# Stat tests
cc_test(
    name = "test_stat",
    size = "small",
    srcs = ["test_stat.cpp"],
    deps = [":test_common"],
)

# URL Stat tests
cc_test(
    name = "test_url_stat",
    size = "small",
    srcs = ["test_url_stat.cpp"],
    deps = [":test_common"],
)

# Span Event tests
cc_test(
    name = "test_span_event",
    size = "small",
    srcs = ["test_span_event.cpp"],
    deps = [":test_common"],
)

# Span tests
cc_test(
    name = "test_span",
    size = "small",
    srcs = ["test_span.cpp"],
    deps = [":test_common"],
)

# Noop tests
cc_test(
    name = "test_noop",
    size = "small",
    srcs = ["test_noop.cpp"],
    deps = [":test_common"],
)

# gRPC tests
cc_test(
    name = "test_grpc",
    size = "small",
    srcs = ["test_grpc.cpp"],
    deps = [":test_gmock"],
)

# gRPC tests with mocks
cc_test(
    name = "test_grpc_with_mocks",
    size = "small",
    srcs = ["test_grpc_with_mocks.cpp"],
    deps = [":test_gmock"],
)

# Test suite for all tests
test_suite(
    name = "all_tests",
    tests = [
        ":test_annotation",
        ":test_cache",
        ":test_callstack",
        ":test_config",
        ":test_grpc",
        ":test_grpc_with_mocks",
        ":test_http",
        ":test_limiter",
        ":test_noop",
        ":test_sampling",
        ":test_span",
        ":test_span_event",
        ":test_sql",
        ":test_stat",
        ":test_url_stat",
    ],
)
