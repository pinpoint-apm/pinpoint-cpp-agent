load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # Apache 2

cc_library(
    name = "mock_collector",
    testonly = True,
    srcs = ["mock_collector.cpp"],
    hdrs = ["mock_collector.h"],
    local_defines = ["PINPOINT_SANITIZER_TIMEOUT_SCALE=3"],
    deps = [
        "//3rd_party:service_grpc_cc",
        "@com_github_grpc_grpc//:grpc++",
    ],
)

cc_test(
    name = "agent_integration_test",
    size = "medium",
    timeout = "moderate",
    srcs = ["test_agent_integration.cpp"],
    data = ["//scripts:tsan.supp"],
    local_defines = ["PINPOINT_SANITIZER_TIMEOUT_SCALE=3"],
    deps = [
        ":mock_collector",
        "//:pinpoint-cpp",
        "//test:c_api_test_helpers",
        "@googletest//:gtest_main",
    ],
)
