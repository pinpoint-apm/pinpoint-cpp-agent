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

# MySQL Demo
cc_binary(
    name = "mysql_demo",
    srcs = ["mysql_demo/db_demo.cpp"],
    copts = [
        "-std=c++14",
    ],
    includes = [
        "mysql_demo",
    ],
    tags = [
        "examples",
        "manual",  # Requires MySQL installation
    ],
    deps = [
        ":example_common",
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
        # Note: MySQL connector dependency needs to be added to WORKSPACE
    ],
)

# CPPTrace Demo
cc_binary(
    name = "cpptrace_demo",
    srcs = ["cpptrace_demo/cpptrace_demo.cpp"],
    tags = [
        "examples",
        "manual",  # Requires cpptrace library
    ],
    deps = [
        ":example_common",
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
        # Note: cpptrace dependency needs to be added to WORKSPACE
    ],
)

# Redis Demo
cc_binary(
    name = "redis_demo",
    srcs = ["redis_demo/redis_example.cpp"],
    tags = [
        "examples",
        "manual",  # Requires redis-plus-plus library
    ],
    deps = [
        ":example_common",
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
        # Note: redis-plus-plus dependency needs to be added to WORKSPACE
    ],
)

# MongoDB Demo
cc_binary(
    name = "mongodb_demo",
    srcs = ["mongodb_demo/mongo_example.cpp"],
    tags = [
        "examples",
        "manual",  # Requires MongoDB C++ driver
    ],
    deps = [
        ":example_common",
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
        # Note: MongoDB driver dependency needs to be added to WORKSPACE
    ],
)

# cURL Demo
cc_binary(
    name = "curl_demo",
    srcs = ["curl_demo/curl_web_example.cpp"],
    tags = [
        "examples",
        "manual",  # Requires libcurl
    ],
    deps = [
        ":example_common",
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
        # Note: libcurl dependency needs to be added to WORKSPACE
    ],
)

# Kafka Demo - Producer
cc_binary(
    name = "kafka_producer",
    srcs = [
        "kafka_demo/kafka_trace_context.h",
        "kafka_demo/kafka_web_producer.cpp",
    ],
    tags = [
        "examples",
        "manual",  # Requires librdkafka
    ],
    deps = [
        ":example_common",
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
        # Note: librdkafka dependency needs to be added to WORKSPACE
    ],
)

# Kafka Demo - Consumer
cc_binary(
    name = "kafka_consumer",
    srcs = [
        "kafka_demo/consumer.cpp",
        "kafka_demo/kafka_trace_context.h",
    ],
    tags = [
        "examples",
        "manual",  # Requires librdkafka
    ],
    deps = [
        ":example_common",
        "//:pinpoint-cpp",
        # Note: librdkafka dependency needs to be added to WORKSPACE
    ],
)

# gRPC Demo - needs proto compilation
# Note: Requires separate BUILD file with proto_library rules

# TodoList Demo
cc_binary(
    name = "todolist_server",
    srcs = ["todolist/todolist_server.cpp"],
    copts = [
        "-std=c++14",
    ],
    tags = [
        "examples",
        "manual",  # Requires MySQL and nlohmann_json
    ],
    deps = [
        ":example_common",
        "//:pinpoint-cpp",
        "//3rd_party:httplib",
        # Note: MySQL connector and nlohmann_json dependencies need to be added to WORKSPACE
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
