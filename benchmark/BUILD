package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # Apache 2

# Numbers from an unoptimized build are misleading, so every target here is
# compiled -O3/NDEBUG regardless of the compilation mode — the same thing
# benchmark/CMakeLists.txt does for this directory. These copts cover the
# benchmark's own translation unit; the two targets that link //:pinpoint-cpp
# also need the LIBRARY optimized, which only `-c opt` can do. See README.md.
BENCHMARK_COPTS = ["-O3"]

BENCHMARK_DEFINES = ["NDEBUG"]

# Header-only against src/: the measured code compiles into the benchmark's own
# translation unit, so these need no library.
cc_binary(
    name = "span_queue_benchmark",
    srcs = ["span_queue_benchmark.cpp"],
    copts = BENCHMARK_COPTS,
    local_defines = BENCHMARK_DEFINES,
    deps = ["//:internal_headers"],
)

cc_binary(
    name = "id_cache_benchmark",
    srcs = ["id_cache_benchmark.cpp"],
    copts = BENCHMARK_COPTS,
    local_defines = BENCHMARK_DEFINES,
    deps = ["//:internal_headers"],
)

cc_binary(
    name = "atomic_shared_ptr_benchmark",
    srcs = ["atomic_shared_ptr_benchmark.cpp"],
    copts = BENCHMARK_COPTS,
    local_defines = BENCHMARK_DEFINES,
    deps = ["//:internal_headers"],
)

cc_binary(
    name = "active_span_benchmark",
    srcs = ["active_span_benchmark.cpp"],
    copts = BENCHMARK_COPTS,
    local_defines = BENCHMARK_DEFINES,
    deps = ["//:internal_headers"],
)

# Self-contained copy of tracer_c.cpp's OwnedHandleRegistry (it lives in an
# anonymous namespace), so it needs no headers from the tree at all.
cc_binary(
    name = "owned_handle_registry_benchmark",
    srcs = ["owned_handle_registry_benchmark.cpp"],
    copts = BENCHMARK_COPTS,
    local_defines = BENCHMARK_DEFINES,
)

# Needs sql.cpp's normalizer, so it links the library.
cc_binary(
    name = "raw_sql_cache_benchmark",
    srcs = ["raw_sql_cache_benchmark.cpp"],
    copts = BENCHMARK_COPTS,
    local_defines = BENCHMARK_DEFINES,
    deps = [
        "//:internal_headers",
        "//:pinpoint-cpp",
    ],
)

# Drives a real AgentImpl, so unlike the component benchmarks above it measures
# code inside the library — build it with `-c opt` or the numbers are several
# times too slow.
cc_binary(
    name = "span_lifecycle_benchmark",
    srcs = ["span_lifecycle_benchmark.cpp"],
    copts = BENCHMARK_COPTS,
    local_defines = BENCHMARK_DEFINES,
    deps = [
        "//:internal_headers",
        "//:pinpoint-cpp",
    ],
)
