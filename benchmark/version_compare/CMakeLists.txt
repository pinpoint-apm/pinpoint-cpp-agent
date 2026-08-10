# Standalone build for the cross-version API benchmark.
#
# This is deliberately NOT wired into the agent's own CMakeLists: it has to be
# buildable against an older checkout whose build system knows nothing about it.
# Point it at a configured agent build tree and it links that version's static
# library.
#
#   cmake -S benchmark/version_compare -B <out> \
#     -DPINPOINT_SOURCE_DIR=<agent worktree> \
#     -DPINPOINT_BUILD_DIR=<configured+built agent build dir> \
#     [-DCMAKE_TOOLCHAIN_FILE=<vcpkg toolchain>] \
#     -DPP_API_V1=ON|OFF [-DBUILD_COLLECTOR=ON]
#
# PP_API_V1=ON selects the v1.1.0 spelling of the public API in pp_compat.h.

cmake_minimum_required(VERSION 3.20)

# Reuse the repository manifest when this standalone project is configured with
# the vcpkg toolchain. Without that toolchain, dependencies are built from the
# pinned FetchContent sources below.
set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Directory containing the Pinpoint vcpkg manifest")
project(pinpoint-version-compare LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Keep benchmarks optimized even when callers explicitly choose a debug build.
if(MSVC)
  add_compile_options(/O2)
else()
  add_compile_options(-O3)
endif()
add_compile_definitions(NDEBUG)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(PP_API_V1 "Target the v1.1.0 public API spelling" OFF)
option(BUILD_COLLECTOR "Also build the counting collector (needs generated protos)" OFF)

if(NOT PINPOINT_SOURCE_DIR)
  message(FATAL_ERROR "PINPOINT_SOURCE_DIR must point at an agent source tree")
endif()
if(NOT PINPOINT_BUILD_DIR)
  message(FATAL_ERROR "PINPOINT_BUILD_DIR must point at a built agent build tree")
endif()

find_library(PINPOINT_LIB
  NAMES pinpoint_cpp
  PATHS ${PINPOINT_BUILD_DIR}
  NO_DEFAULT_PATH REQUIRED)

find_package(Threads REQUIRED)
set(PINPOINT_FORCE_FETCHCONTENT OFF)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/PinpointDependencies.cmake")

set(PINPOINT_DEP_LIBS
  gRPC::grpc++
  protobuf::libprotobuf
  yaml-cpp::yaml-cpp
  fmt::fmt
  Threads::Threads)

add_executable(api_benchmark api_benchmark.cpp)
target_include_directories(api_benchmark PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PINPOINT_SOURCE_DIR}/include)
target_link_libraries(api_benchmark PRIVATE ${PINPOINT_LIB} ${PINPOINT_DEP_LIBS})
if(PP_API_V1)
  target_compile_definitions(api_benchmark PRIVATE PP_API_V1=1)
endif()

# Phase-2 load-test server: same shim, workload arrives over HTTP from
# test/e2e/fixed_rps_test.py. httplib comes from the repository this file lives
# in, so both variants embed the identical HTTP stack.
add_executable(bench_http_server bench_http_server.cpp)
target_include_directories(bench_http_server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../3rd_party
  ${PINPOINT_SOURCE_DIR}/include)
target_link_libraries(bench_http_server PRIVATE ${PINPOINT_LIB} ${PINPOINT_DEP_LIBS})
if(PP_API_V1)
  target_compile_definitions(bench_http_server PRIVATE PP_API_V1=1)
endif()

# The collector is version-independent and only needs to be built once. It links
# the agent's static library purely to pick up the generated protobuf/gRPC
# symbols rather than regenerating them.
if(BUILD_COLLECTOR)
  add_executable(bench_collector bench_collector.cpp)
  target_include_directories(bench_collector PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PINPOINT_SOURCE_DIR}/include
    ${PINPOINT_BUILD_DIR})
  target_link_libraries(bench_collector PRIVATE ${PINPOINT_LIB} ${PINPOINT_DEP_LIBS})
endif()
