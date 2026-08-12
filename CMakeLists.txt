cmake_minimum_required(VERSION 3.21)

project(pinpoint-cpp VERSION 2.0.0)

option(BUILD_TESTING      "Build tests"                ON)
option(BUILD_EXAMPLES     "Build examples"             ON)
option(BUILD_SHARED_LIBS  "Build as a shared library"  ON)
option(BUILD_STATIC_LIBS  "Build as a static library"  ON)
option(BUILD_COVERAGE     "Enable coverage instrumentation (Clang/LLVM or GCC)" OFF)
option(BUILD_BENCHMARKS   "Build standalone microbenchmarks" OFF)
option(BUILD_PROFILING    "Build with symbols and reliable sampling-profiler stacks" OFF)
set(SANITIZE "" CACHE STRING
  "Enable a sanitizer: address, thread, or undefined (empty = off)")
set_property(CACHE SANITIZE PROPERTY STRINGS
  "" "address" "thread" "undefined")

# Sanitized builds normally compile third-party code with the same
# instrumentation. The dependency module consumes this flag when it selects
# between the active vcpkg toolchain and FetchContent.
set(PINPOINT_FORCE_FETCHCONTENT OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Sanitizers
# =============================================================================
# This block must stay ahead of dependency resolution below. add_compile_options()
# only affects targets created after the call, so enabling the sanitizer later
# would leave every FetchContent-built dependency uninstrumented, which is worse
# than it sounds: the instrumented copies of inline C++ symbols (std::string_view,
# std::char_traits, ...) are weak, so the dynamic linker interposes them into the
# uninstrumented gRPC libraries. Those instrumented inlines then execute on stack
# frames ASan has no descriptor for and it misattributes leftover shadow poison,
# reporting use-after-poison inside gRPC internals. Such reports are not
# suppressible: ASan's suppression file only understands interceptor_via_fun,
# interceptor_via_lib and odr_violation, so it cannot silence an error raised by a
# direct instrumented load check. Giving the sanitizer a whole-program view is the
# only reliable fix.
# =============================================================================
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
  else()
    message(FATAL_ERROR
      "Unknown SANITIZE value '${SANITIZE}'. Use address, thread, or undefined.")
  endif()

  # A vcpkg-provided gRPC/Abseil/protobuf is prebuilt and therefore cannot be
  # instrumented, which reintroduces the boundary described above. Force the
  # FetchContent provider so dependencies are rebuilt with the flags below.
  set(PINPOINT_FORCE_FETCHCONTENT ON)
  message(STATUS
    "Sanitized dependencies: gRPC/Protobuf/absl/yaml-cpp/fmt are built "
    "from source with ${SANITIZE} instrumentation (first build is slow)")

  # The instrumentation above also covers protoc and grpc_cpp_plugin, which this
  # build then runs to generate the .pb.cc sources. Protobuf keeps an over-aligned
  # ThreadCache in thread_local storage, an alignment macOS TLS does not honour, so
  # an instrumented protoc aborts before emitting anything. Drop the alignment check
  # for the fetched dependencies; see scripts/ubsan.ignorelist. Compile-only, and
  # Clang-only: GCC has no equivalent flag.
  if(SANITIZE MATCHES "undefined" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_pinpoint_ubsan_ignorelist
      -fsanitize-ignorelist=${CMAKE_CURRENT_SOURCE_DIR}/scripts/ubsan.ignorelist)
  endif()

  # Retain symbolized, complete stacks in sanitizer reports and apply the same
  # instrumentation to both compile and link (the runtime is pulled in at link).
  add_compile_options(-g -fno-omit-frame-pointer ${_pinpoint_sanitize_flags}
    ${_pinpoint_ubsan_ignorelist})
  set(_pinpoint_sanitize_link_flags ${_pinpoint_sanitize_flags})
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Ubuntu's Clang defaults to libgcc, but ARM64 libgcc does not provide every
    # compiler builtin emitted by UBSan (for example __muloti4). Use the
    # compiler-rt builtins shipped alongside Clang's sanitizer runtimes.
    list(APPEND _pinpoint_sanitize_link_flags --rtlib=compiler-rt)
  endif()
  add_link_options(${_pinpoint_sanitize_link_flags})
  message(STATUS "Sanitizer enabled: ${SANITIZE}")
endif()

# =============================================================================
# Dependency resolution
# =============================================================================
# Only an active vcpkg toolchain may provide packages. Without it, pinned
# dependency sources are always built through FetchContent; system packages and
# other package managers are intentionally ignored.
# =============================================================================
include(cmake/PinpointDependencies.cmake)

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
# `#include "v1/..."` sites don't change. Release tarballs bundle the populated
# submodule (see release.yml), so an empty directory means a git checkout that
# has not initialized its submodules yet.
set(PP_GRPC_IDL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/pinpoint-grpc-idl)

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
  endif()

  if(NOT EXISTS ${PP_GRPC_IDL_DIR}/proto/v1/Service.proto)
    message(FATAL_ERROR
      "pinpoint-grpc-idl protos are missing at ${PP_GRPC_IDL_DIR}. "
      "Clone with submodules (git clone --recurse-submodules), run "
      "`git submodule update --init --recursive`, or use a release tarball "
      "(which bundles them).")
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

# Shared by the test / example / benchmark subtrees: the library name to
# link against, and the platform threading library.
if(BUILD_SHARED_LIBS)
  set(PINPOINT_CPP_LIBRARY pinpoint_cpp)
else()
  set(PINPOINT_CPP_LIBRARY pinpoint_cpp-static)
endif()
find_package(Threads REQUIRED)

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
  set(LLVM_COVERAGE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/coverage" CACHE PATH
      "Directory for llvm-cov profiles and reports")

  # A custom target does not implicitly build CTest executables. Test targets
  # register themselves on this global property (add_pinpoint_test and the it
  # suite); opt-in live test applications stay excluded.
  get_property(_pinpoint_test_targets GLOBAL PROPERTY PINPOINT_TEST_TARGETS)

  add_custom_target(coverage
    COMMAND "${CMAKE_COMMAND}"
      "-DCTEST_EXECUTABLE=${CMAKE_CTEST_COMMAND}"
      "-DLLVM_COV_EXECUTABLE=${LLVM_COV_EXECUTABLE}"
      "-DLLVM_PROFDATA_EXECUTABLE=${LLVM_PROFDATA_EXECUTABLE}"
      "-DCOVERAGE_BINARY=$<TARGET_FILE:${PINPOINT_CPP_LIBRARY}>"
      "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
      "-DOUTPUT_DIR=${LLVM_COVERAGE_OUTPUT_DIR}"
      "-DCOVERAGE_CONFIG=$<CONFIG>"
      "-DCOVERAGE_IGNORE_REGEX=(^|/)(test|3rd_party|build|_deps)(/|$)"
      -P "${PROJECT_SOURCE_DIR}/cmake/RunLlvmCoverage.cmake"
    USES_TERMINAL
    VERBATIM)

  add_dependencies(coverage
    ${PINPOINT_CPP_LIBRARY}
    ${_pinpoint_test_targets})
endif()
