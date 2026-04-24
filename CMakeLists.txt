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
option(BUILD_COVERAGE     "Enable coverage (GCC only)" OFF)

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
)

# Extra protobuf import dirs are only needed when gRPC was fetched from source,
# since bundled protobuf headers live inside the gRPC source tree.
set(PP_PROTOBUF_IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR})
if(DEFINED grpc_SOURCE_DIR)
  list(APPEND PP_PROTOBUF_IMPORT_DIRS ${grpc_SOURCE_DIR}/third_party/protobuf/src)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Wthread-safety -Wno-unused-parameter")
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

set(PROTOS v1/Annotation.proto
           v1/Cmd.proto
           v1/Service.proto
           v1/Span.proto
           v1/Stat.proto
           v1/ThreadDump.proto)

set(SRCS src/agent.cpp
         src/annotation.cpp
         src/config.cpp
         src/grpc.cpp
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
  target_include_directories(pinpoint_cpp INTERFACE "$<INSTALL_INTERFACE:include/>")
  set_target_properties(pinpoint_cpp PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
  target_link_libraries(pinpoint_cpp PUBLIC ${LINK_LIBS})
  install(TARGETS pinpoint_cpp LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if(BUILD_STATIC_LIBS)
  add_library(pinpoint_cpp-static STATIC ${SRCS} ${PROTOS})
  set_target_properties(pinpoint_cpp-static PROPERTIES OUTPUT_NAME pinpoint_cpp)
  target_include_directories(pinpoint_cpp-static INTERFACE "$<INSTALL_INTERFACE:include/>")
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
