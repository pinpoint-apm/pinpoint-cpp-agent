cmake_minimum_required(VERSION 3.21)

set(CMAKE_VERBOSE_MAKEFILE ON)

# =============================================================================
# ccache auto-detection
# =============================================================================
# If ccache is available on PATH, wire it up as the compiler launcher.
# Disable with -DUSE_CCACHE=OFF.
# =============================================================================
option(USE_CCACHE "Use ccache as compiler launcher if available" ON)
if(USE_CCACHE)
  find_program(CCACHE_PROGRAM ccache)
  if(CCACHE_PROGRAM)
    message(STATUS "ccache found: ${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  else()
    message(STATUS "ccache not found, continuing without compiler launcher")
  endif()
endif()

project(pinpoint-cpp VERSION 1.1.0)

option(BUILD_TESTING      "Build tests"                ON)
option(BUILD_EXAMPLES     "Build examples"             ON)
option(BUILD_SHARED_LIBS  "Build as a shared library"  ON)
option(BUILD_STATIC_LIBS  "Build as a static library"  ON)
option(BUILD_COVERAGE     "Enable coverage instrumentation (Clang/LLVM or GCC)" OFF)
option(BUILD_BENCHMARKS   "Build standalone microbenchmarks" OFF)
option(BUILD_PROFILING    "Build with symbols and reliable sampling-profiler stacks" OFF)
set(SANITIZE "" CACHE STRING
  "Enable a sanitizer: address, thread, undefined, or address+undefined (empty = off)")
set_property(CACHE SANITIZE PROPERTY STRINGS
  "" "address" "thread" "undefined" "address+undefined")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# =============================================================================
# Dependency resolution
# =============================================================================
# Packages are located via find_package(CONFIG). Any package manager that
# integrates with CMake (vcpkg, Conan, system packages, Nix, Spack, ...) can
# be plugged in at configure time via CMAKE_TOOLCHAIN_FILE or CMAKE_PREFIX_PATH.
# If a package cannot be found, FetchContent is used to build it from source.
#
# To keep the rest of this file toolchain-agnostic, FetchContent-built targets
# are aliased to the same namespaced names that find_package(CONFIG) provides
# (e.g. gRPC::grpc++).
# =============================================================================

find_package(Protobuf CONFIG QUIET)
find_package(gRPC     CONFIG QUIET)
find_package(absl     CONFIG QUIET)

if(NOT (gRPC_FOUND AND Protobuf_FOUND AND absl_FOUND))
  message(STATUS "gRPC/Protobuf/absl not found via toolchain, fetching from source...")

  FetchContent_Declare(
    gRPC
    GIT_REPOSITORY https://github.com/grpc/grpc.git
    GIT_TAG        v1.76.0
  )
  set(FETCHCONTENT_QUIET OFF)
  set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(gRPC_BUILD_CSHARP_EXT OFF CACHE BOOL "" FORCE)
  set(gRPC_ABSL_PROVIDER "module" CACHE STRING "" FORCE)
  set(gRPC_PROTOBUF_PROVIDER "module" CACHE STRING "" FORCE)
  set(gRPC_ZLIB_PROVIDER "module" CACHE STRING "" FORCE)
  set(gRPC_CARES_PROVIDER "module" CACHE STRING "" FORCE)
  set(gRPC_RE2_PROVIDER "module" CACHE STRING "" FORCE)
  set(gRPC_SSL_PROVIDER "module" CACHE STRING "" FORCE)
  set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
  set(utf8_range_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(gRPC)

  # Alias raw targets to the namespaced names find_package(CONFIG) would create,
  # so the rest of this file can use a single target naming convention.
  foreach(_pp_grpc_target IN ITEMS
            grpc grpc++ grpc++_reflection gpr
            grpc_unsecure grpc++_unsecure
            grpc++_error_details grpcpp_channelz)
    if(TARGET ${_pp_grpc_target} AND NOT TARGET gRPC::${_pp_grpc_target})
      add_library(gRPC::${_pp_grpc_target} ALIAS ${_pp_grpc_target})
    endif()
  endforeach()
  if(TARGET grpc_cpp_plugin AND NOT TARGET gRPC::grpc_cpp_plugin)
    add_executable(gRPC::grpc_cpp_plugin ALIAS grpc_cpp_plugin)
  endif()

  include(${grpc_SOURCE_DIR}/third_party/protobuf/cmake/protobuf-generate.cmake)
else()
  message(STATUS "Using toolchain-provided gRPC/Protobuf/absl")
  include(${Protobuf_DIR}/../protobuf-generate.cmake OPTIONAL)
  if(NOT COMMAND protobuf_generate)
    include(FindProtobuf)
  endif()
endif()

find_package(yaml-cpp CONFIG QUIET)
if(NOT yaml-cpp_FOUND)
  message(STATUS "yaml-cpp not found via toolchain, fetching from source...")
  FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0
  )
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(yaml-cpp)
endif()

find_package(fmt CONFIG QUIET)
if(NOT fmt_FOUND)
  message(STATUS "fmt not found via toolchain, fetching from source...")
  FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        11.2.0
  )
  FetchContent_MakeAvailable(fmt)
endif()

list(APPEND LINK_LIBS
  protobuf::libprotobuf
  gRPC::grpc++
  yaml-cpp::yaml-cpp
  fmt::fmt
  absl::strings
  absl::random_random
)

# The pinpoint-grpc-idl submodule lays out protos as proto/v1/*.proto; adding
# .../proto to the import path keeps generated headers at v1/*.pb.h so existing
# `#include "v1/..."` sites don't change.
#
# Commit pinned here MUST match the submodule SHA (see 3rd_party/pinpoint-grpc-idl).
# It is only consulted when the submodule directory is empty AND the repo is not
# a git checkout (e.g. building from a GitHub auto-generated source tarball).
set(PP_GRPC_IDL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/pinpoint-grpc-idl)
set(PP_GRPC_IDL_URL "https://github.com/pinpoint-apm/pinpoint-grpc-idl.git")
set(PP_GRPC_IDL_COMMIT "48978376de3406d356c4db3f32e40125b2050322")

if(NOT EXISTS ${PP_GRPC_IDL_DIR}/proto/v1/Service.proto)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git)
    # In-tree git checkout: initialize the submodule.
    find_package(Git QUIET)
    if(NOT Git_FOUND)
      message(FATAL_ERROR "pinpoint-grpc-idl submodule is missing and git was "
              "not found. Install git or run:\n"
              "  git submodule update --init --recursive")
    endif()
    message(STATUS "pinpoint-grpc-idl submodule not initialized; running "
            "`git submodule update --init --recursive`")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      RESULT_VARIABLE _pp_gm_rc)
    if(NOT _pp_gm_rc EQUAL 0)
      message(FATAL_ERROR "`git submodule update --init` failed (rc=${_pp_gm_rc})")
    endif()
  else()
    # No .git (e.g. source tarball): fetch the pinned commit via FetchContent.
    message(STATUS "No .git directory; fetching pinpoint-grpc-idl "
            "@${PP_GRPC_IDL_COMMIT} via FetchContent")
    include(FetchContent)
    FetchContent_Declare(
      pinpoint_grpc_idl
      GIT_REPOSITORY ${PP_GRPC_IDL_URL}
      GIT_TAG        ${PP_GRPC_IDL_COMMIT}
      GIT_SHALLOW    FALSE   # commit SHAs are not reachable via shallow fetch
      SOURCE_DIR     ${PP_GRPC_IDL_DIR}
    )
    FetchContent_MakeAvailable(pinpoint_grpc_idl)
  endif()

  if(NOT EXISTS ${PP_GRPC_IDL_DIR}/proto/v1/Service.proto)
    message(FATAL_ERROR
      "pinpoint-grpc-idl protos are still missing at ${PP_GRPC_IDL_DIR}. "
      "Network required, or populate the directory manually.")
  endif()
endif()
set(PP_PROTOBUF_IMPORT_DIRS ${PP_GRPC_IDL_DIR}/proto)
# Extra protobuf import dirs are only needed when gRPC was fetched from source,
# since bundled protobuf headers live inside the gRPC source tree.
if(DEFINED grpc_SOURCE_DIR)
  list(APPEND PP_PROTOBUF_IMPORT_DIRS ${grpc_SOURCE_DIR}/third_party/protobuf/src)
endif()

set(PINPOINT_LLVM_COVERAGE_ENABLED OFF)

if(BUILD_PROFILING)
  if(BUILD_COVERAGE)
    message(FATAL_ERROR
      "BUILD_PROFILING and BUILD_COVERAGE cannot be enabled together")
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR
      "BUILD_PROFILING currently supports Clang/AppleClang and GCC")
  endif()

  # Keep production-like optimization while preserving symbolized, complete
  # call stacks for xctrace Time Profiler and Linux perf.
  add_compile_options(-g -fno-omit-frame-pointer -fno-optimize-sibling-calls)

  if(NOT CMAKE_CONFIGURATION_TYPES AND
     NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
    message(WARNING
      "BUILD_PROFILING is intended for Release or RelWithDebInfo builds; "
      "current CMAKE_BUILD_TYPE is '${CMAKE_BUILD_TYPE}'")
  endif()
  message(STATUS
    "Sampling-profiler build enabled: debug symbols and frame pointers retained")
endif()

if(SANITIZE)
  if(BUILD_COVERAGE OR BUILD_PROFILING)
    message(FATAL_ERROR
      "SANITIZE cannot be combined with BUILD_COVERAGE or BUILD_PROFILING")
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "SANITIZE currently supports Clang/AppleClang and GCC")
  endif()

  if(SANITIZE STREQUAL "address")
    set(_pinpoint_sanitize_flags -fsanitize=address)
  elseif(SANITIZE STREQUAL "thread")
    set(_pinpoint_sanitize_flags -fsanitize=thread)
  elseif(SANITIZE STREQUAL "undefined")
    set(_pinpoint_sanitize_flags -fsanitize=undefined -fno-sanitize-recover=undefined)
  elseif(SANITIZE STREQUAL "address+undefined")
    set(_pinpoint_sanitize_flags
      -fsanitize=address,undefined -fno-sanitize-recover=undefined)
  else()
    message(FATAL_ERROR
      "Unknown SANITIZE value '${SANITIZE}'. "
      "Use address, thread, undefined, or address+undefined.")
  endif()

  # Retain symbolized, complete stacks in sanitizer reports and apply the same
  # instrumentation to both compile and link (the runtime is pulled in at link).
  add_compile_options(-g -fno-omit-frame-pointer ${_pinpoint_sanitize_flags})
  add_link_options(${_pinpoint_sanitize_flags})
  message(STATUS "Sanitizer enabled: ${SANITIZE}")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Wthread-safety -Wno-unused-parameter -Wno-gcc-compat -Wno-gnu-zero-variadic-macro-arguments")

  if(BUILD_COVERAGE)
    if(NOT BUILD_TESTING)
      message(FATAL_ERROR "BUILD_COVERAGE requires BUILD_TESTING=ON")
    endif()

    # Ask Clang where its companion tools live first. This also resolves the
    # Xcode toolchain behind macOS /usr/bin compiler shims. PATH remains a
    # fallback and both cache entries can be overridden explicitly.
    get_filename_component(_llvm_toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    execute_process(
      COMMAND "${CMAKE_CXX_COMPILER}" --print-prog-name=llvm-cov
      OUTPUT_VARIABLE _llvm_cov_from_compiler
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    execute_process(
      COMMAND "${CMAKE_CXX_COMPILER}" --print-prog-name=llvm-profdata
      OUTPUT_VARIABLE _llvm_profdata_from_compiler
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)

    set(_llvm_tool_hints "${_llvm_toolchain_bin}")
    if(EXISTS "${_llvm_cov_from_compiler}")
      get_filename_component(_llvm_cov_bin "${_llvm_cov_from_compiler}" DIRECTORY)
      list(APPEND _llvm_tool_hints "${_llvm_cov_bin}")
    endif()
    if(EXISTS "${_llvm_profdata_from_compiler}")
      get_filename_component(_llvm_profdata_bin
        "${_llvm_profdata_from_compiler}" DIRECTORY)
      list(APPEND _llvm_tool_hints "${_llvm_profdata_bin}")
    endif()
    list(REMOVE_DUPLICATES _llvm_tool_hints)

    string(REGEX MATCH "^[0-9]+" _llvm_tool_major
           "${CMAKE_CXX_COMPILER_VERSION}")
    set(_llvm_cov_names llvm-cov)
    set(_llvm_profdata_names llvm-profdata)
    if(NOT "${_llvm_tool_major}" STREQUAL "")
      # Debian-style LLVM packages may expose only version-suffixed binaries.
      list(PREPEND _llvm_cov_names "llvm-cov-${_llvm_tool_major}")
      list(PREPEND _llvm_profdata_names "llvm-profdata-${_llvm_tool_major}")
    endif()

    find_program(LLVM_COV_EXECUTABLE
      NAMES ${_llvm_cov_names}
      HINTS ${_llvm_tool_hints}
      DOC "Path to llvm-cov")
    find_program(LLVM_PROFDATA_EXECUTABLE
      NAMES ${_llvm_profdata_names}
      HINTS ${_llvm_tool_hints}
      DOC "Path to llvm-profdata")

    if(NOT LLVM_COV_EXECUTABLE OR NOT LLVM_PROFDATA_EXECUTABLE)
      message(FATAL_ERROR
        "BUILD_COVERAGE with Clang requires llvm-cov and llvm-profdata. "
        "Install the matching LLVM tools or set LLVM_COV_EXECUTABLE and "
        "LLVM_PROFDATA_EXECUTABLE explicitly.")
    endif()

    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate -fcoverage-mapping)
    set(PINPOINT_LLVM_COVERAGE_ENABLED ON)

    mark_as_advanced(LLVM_COV_EXECUTABLE LLVM_PROFDATA_EXECUTABLE)
    message(STATUS "LLVM coverage enabled")
    message(STATUS "  llvm-cov: ${LLVM_COV_EXECUTABLE}")
    message(STATUS "  llvm-profdata: ${LLVM_PROFDATA_EXECUTABLE}")
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter -pthread -Wl,--no-as-needed")

  if(BUILD_COVERAGE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lgcov")
  endif()
endif()

file (STRINGS "build_number.txt" BUILD_NUMBER)
configure_file(version.h.in version.h)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(include)
include_directories(src)
include_directories(3rd_party)
include(GNUInstallDirs)

set(PROTOS ${PP_GRPC_IDL_DIR}/proto/v1/Annotation.proto
           ${PP_GRPC_IDL_DIR}/proto/v1/Cmd.proto
           ${PP_GRPC_IDL_DIR}/proto/v1/Service.proto
           ${PP_GRPC_IDL_DIR}/proto/v1/Span.proto
           ${PP_GRPC_IDL_DIR}/proto/v1/Stat.proto
           ${PP_GRPC_IDL_DIR}/proto/v1/ThreadDump.proto)

set(SRCS src/agent.cpp
         src/annotation.cpp
         src/config.cpp
         src/object_name.cpp
         src/grpc.cpp
         src/grpc_builders.cpp
         src/limiter.cpp
         src/logging.cpp
         src/noop.cpp
         src/sampling.cpp
         src/span.cpp
         src/span_event.cpp
         src/stat.cpp
         src/url_stat.cpp
         src/http.cpp
         src/cache.cpp
         src/utility.cpp
         src/sql.cpp
         src/tracer_c.cpp
         3rd_party/MurmurHash3.cpp)

protobuf_generate(
  OUT_VAR PP_GENERATED_PROTO_SRCS
  LANGUAGE cpp
  IMPORT_DIRS ${PP_PROTOBUF_IMPORT_DIRS}
  PROTOS ${PROTOS}
)
protobuf_generate(
  OUT_VAR PP_GENERATED_GRPC_SRCS
  LANGUAGE grpc
  PLUGIN protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
  PLUGIN_OPTIONS generate_mock_code=true
  IMPORT_DIRS ${PP_PROTOBUF_IMPORT_DIRS}
  PROTOS ${PROTOS}
)
list(APPEND SRCS ${PP_GENERATED_PROTO_SRCS} ${PP_GENERATED_GRPC_SRCS})

if(BUILD_SHARED_LIBS)
  add_library(pinpoint_cpp SHARED ${SRCS} ${PROTOS})
  target_include_directories(pinpoint_cpp INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include/>")
  set_target_properties(pinpoint_cpp PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
  target_link_libraries(pinpoint_cpp PUBLIC ${LINK_LIBS})
  install(TARGETS pinpoint_cpp LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if(BUILD_STATIC_LIBS)
  add_library(pinpoint_cpp-static STATIC ${SRCS} ${PROTOS})
  set_target_properties(pinpoint_cpp-static PROPERTIES OUTPUT_NAME pinpoint_cpp)
  target_include_directories(pinpoint_cpp-static INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include/>")
  target_link_libraries(pinpoint_cpp-static ${LINK_LIBS})
  if(APPLE)
    # Force-load absl::strings to avoid undefined-reference issues on macOS.
    target_link_options(pinpoint_cpp-static PUBLIC "-Wl,-force_load,$<TARGET_FILE:absl::strings>")
  endif()
  install(TARGETS pinpoint_cpp-static ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

install(DIRECTORY include/pinpoint DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Testing
if (BUILD_TESTING)
  include(CTest)
  add_subdirectory(test)
endif()

if (BUILD_EXAMPLES)
  add_subdirectory(example)
endif()

if (BUILD_BENCHMARKS)
  add_subdirectory(benchmark)
endif()

# LLVM source-based coverage target. It builds every in-tree test executable,
# runs CTest with a collision-safe profile filename, merges the raw profiles,
# and writes both a text summary and an HTML report.
if(PINPOINT_LLVM_COVERAGE_ENABLED)
  if(BUILD_SHARED_LIBS)
    set(_pinpoint_coverage_binary_target pinpoint_cpp)
  elseif(BUILD_STATIC_LIBS)
    set(_pinpoint_coverage_binary_target pinpoint_cpp-static)
  else()
    message(FATAL_ERROR
      "BUILD_COVERAGE requires BUILD_SHARED_LIBS or BUILD_STATIC_LIBS")
  endif()

  set(LLVM_COVERAGE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/coverage" CACHE PATH
      "Directory for llvm-cov profiles and reports")

  # A custom target does not implicitly build CTest executables. Collect the
  # in-tree targets whose names match registered CTest tests. Link dependencies
  # are then built normally, while opt-in live test applications stay excluded.
  function(_pinpoint_collect_test_targets directory result)
    set(_collected_targets)

    get_property(_directory_tests DIRECTORY "${directory}" PROPERTY TESTS)
    foreach(_test IN LISTS _directory_tests)
      if(TARGET "${_test}")
        get_target_property(_target_source_dir "${_test}" SOURCE_DIR)
        file(TO_CMAKE_PATH "${_target_source_dir}/" _target_source_dir)
        file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/test/" _test_source_dir)
        string(FIND "${_target_source_dir}" "${_test_source_dir}" _test_prefix)

        if(_test_prefix EQUAL 0)
          list(APPEND _collected_targets "${_test}")
        endif()
      endif()
    endforeach()

    get_property(_subdirectories DIRECTORY "${directory}"
                 PROPERTY SUBDIRECTORIES)
    foreach(_subdirectory IN LISTS _subdirectories)
      _pinpoint_collect_test_targets("${_subdirectory}" _subdirectory_targets)
      list(APPEND _collected_targets ${_subdirectory_targets})
    endforeach()

    list(REMOVE_DUPLICATES _collected_targets)
    set(${result} "${_collected_targets}" PARENT_SCOPE)
  endfunction()

  _pinpoint_collect_test_targets("${PROJECT_SOURCE_DIR}/test"
                                  _pinpoint_test_targets)

  add_custom_target(coverage
    COMMAND "${CMAKE_COMMAND}"
      "-DCTEST_EXECUTABLE=${CMAKE_CTEST_COMMAND}"
      "-DLLVM_COV_EXECUTABLE=${LLVM_COV_EXECUTABLE}"
      "-DLLVM_PROFDATA_EXECUTABLE=${LLVM_PROFDATA_EXECUTABLE}"
      "-DCOVERAGE_BINARY=$<TARGET_FILE:${_pinpoint_coverage_binary_target}>"
      "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
      "-DOUTPUT_DIR=${LLVM_COVERAGE_OUTPUT_DIR}"
      "-DCOVERAGE_CONFIG=$<CONFIG>"
      "-DCOVERAGE_IGNORE_REGEX=(^|/)(test|3rd_party|build|_deps)(/|$)"
      -P "${PROJECT_SOURCE_DIR}/cmake/RunLlvmCoverage.cmake"
    USES_TERMINAL
    VERBATIM)

  add_dependencies(coverage
    ${_pinpoint_coverage_binary_target}
    ${_pinpoint_test_targets})
endif()
