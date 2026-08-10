package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # Apache 2

# Test library with common dependencies (gtest and gmock tests alike;
# gtest_main provides both)
cc_library(
    name = "test_common",
    testonly = True,
    hdrs = [
        "mock_agent_service.h",
        "mock_helpers.h",
    ],
    includes = [
        ".",
        "../src",
    ],
    deps = [
        "//:pinpoint-cpp",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "c_api_test_helpers",
    testonly = True,
    hdrs = ["c_api_test_helpers.h"],
    deps = ["//:pinpoint-cpp"],
)

# Unit tests that follow the one-source/one-binary pattern.
_TESTS = [
    "test_agent_with_mocks",
    "test_annotation",
    "test_atomic_shared_ptr",
    "test_cache",
    "test_callstack",
    "test_config",
    "test_fork",
    "test_grpc",
    "test_grpc_with_mocks",
    "test_http",
    "test_limiter",
    "test_logging",
    "test_noop",
    "test_object_name",
    "test_sampling",
    "test_sharded_bounded_queue",
    "test_span",
    "test_span_event",
    "test_sql",
    "test_stat",
    "test_url_stat",
    "test_utility",
]

[
    cc_test(
        name = n,
        size = "small",
        srcs = [n + ".cpp"],
        deps = [":test_common"],
    )
    for n in _TESTS
]

# C API tests need the extra helper header.
cc_test(
    name = "test_tracer_c",
    size = "small",
    srcs = ["test_tracer_c.cpp"],
    deps = [
        ":c_api_test_helpers",
        ":test_common",
    ],
)

# Test suite for all tests
test_suite(
    name = "all_tests",
    tests = [":" + t for t in _TESTS] + [
        ":test_tracer_c",
        "//test/it:agent_integration_test",
    ],
)
