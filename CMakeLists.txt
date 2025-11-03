cmake_minimum_required(VERSION 3.20)

set(CMAKE_VERBOSE_MAKEFILE on)

include(CMakeDependentOption)
include(FetchContent)

project(pinpoint-cpp VERSION 1.0.0)

option(BUILD_TESTING "Build tests" ON)
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_SHARED_LIBS "Build as a shared library" OFF)
option(BUILD_STATIC_LIBS "Build as a static library" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Fetch gRPC (includes Protobuf and Abseil as submodules)
FetchContent_Declare(
  gRPC
  GIT_REPOSITORY https://github.com/grpc/grpc.git
  GIT_TAG        v1.66.0
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
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(utf8_range_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(gRPC)

# Include protobuf-generate module for protobuf_generate() function
include(${grpc_SOURCE_DIR}/third_party/protobuf/cmake/protobuf-generate.cmake)

list(APPEND LINK_LIBS protobuf::libprotobuf)
list(APPEND LINK_LIBS grpc grpc++ grpc++_reflection)
list(APPEND LINK_LIBS gpr grpc_unsecure grpc++_unsecure)
list(APPEND LINK_LIBS grpc++_error_details grpcpp_channelz)
list(APPEND LINK_LIBS 
  absl::strings 
  absl::strings_internal
  absl::str_format_internal
  absl::string_view
  absl::cord
  absl::log
  absl::log_internal_message
  absl::log_internal_check_op
  absl::log_internal_conditions
  absl::log_internal_format
  absl::log_internal_log_sink_set
  absl::log_internal_nullguard
  absl::log_internal_proto
  absl::log_globals
  absl::int128
)

# Fetch yaml-cpp
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG        0.8.0
)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)
list(APPEND LINK_LIBS yaml-cpp::yaml-cpp)

# Fetch fmt
FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG        11.0.2
)
FetchContent_MakeAvailable(fmt)
list(APPEND LINK_LIBS fmt::fmt)

# Fetch spdlog
FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.14.1
)
FetchContent_MakeAvailable(spdlog)
list(APPEND LINK_LIBS spdlog::spdlog)

FetchContent_Declare(
  cpptrace
  GIT_REPOSITORY https://github.com/jeremy-rifkin/cpptrace.git
  GIT_TAG        v1.0.4
)
FetchContent_MakeAvailable(cpptrace)
list(APPEND LINK_LIBS cpptrace::cpptrace)

# Fetch GoogleTest (for testing)
if (BUILD_TESTING)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
  )
  # For Windows: Prevent overriding the parent project's compiler/linker settings
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Weverything \
    -Wno-c++98-compat \
    -Wno-c++98-compat-pedantic \
    -Wno-c++98-compat-bind-to-temporary-copy \
    -Wno-weak-vtables \
    -Wno-exit-time-destructors \
    -Wno-global-constructors \
    -Wno-padded \
    -Wno-float-equal \
    -Wno-sign-conversion \
    -Wno-unused-parameter \
    -Wno-missing-variable-declarations \
    -Wno-missing-prototypes \
    -Wno-reserved-id-macro \
    -Wno-old-style-cast \
    -Wno-conversion \
    -Wno-suggest-destructor-override \
    -Wno-shadow-field \
    -Wno-unknown-warning-option \
    -Wno-poison-system-directories \
    -Wno-vla-extension \
    -Wno-c++20-compat \
    -Wno-unreachable-code-return \
    -Wno-switch-enum \
    -Wthread-safety")
    #-stdlib=libc++")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -stdlib=libc++")
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra \
    -Wno-unused-parameter \
    -std=c++11 -pthread \
    -Wl,--no-as-needed")
    
  if (BUILD_COVERAGE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lgcov")
  endif()  
endif()

file (STRINGS "build_number.txt" BUILD_NUMBER)
configure_file(version.h.in version.h)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(include)
include_directories(src)
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
         src/MurmurHash3.cpp)

if (BUILD_SHARED_LIBS)
  add_library(pinpoint_cpp SHARED ${SRCS} ${PROTOS})
  target_include_directories(pinpoint_cpp INTERFACE "$<INSTALL_INTERFACE:include/>")
  set_target_properties(pinpoint_cpp PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
  target_link_libraries(pinpoint_cpp PUBLIC ${LINK_LIBS})
  install(TARGETS pinpoint_cpp LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  
  protobuf_generate(
    TARGET pinpoint_cpp
    LANGUAGE cpp
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR} ${grpc_SOURCE_DIR}/third_party/protobuf/src
  )

  protobuf_generate(
    TARGET pinpoint_cpp
    LANGUAGE grpc
    PLUGIN protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN_OPTIONS generate_mock_code=true
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR} ${grpc_SOURCE_DIR}/third_party/protobuf/src
  )
endif()

if (BUILD_STATIC_LIBS)
  add_library(pinpoint_cpp-static STATIC ${SRCS} ${PROTOS})
  set_target_properties(pinpoint_cpp-static PROPERTIES OUTPUT_NAME pinpoint_cpp)
  target_include_directories(pinpoint_cpp-static INTERFACE "$<INSTALL_INTERFACE:include/>")
  target_link_libraries(pinpoint_cpp-static ${LINK_LIBS})
  # Force load all symbols from absl::strings to fix undefined reference issues
  if(APPLE)
    target_link_options(pinpoint_cpp-static PUBLIC "-Wl,-force_load,$<TARGET_FILE:absl::strings>")
  endif()
  install(TARGETS pinpoint_cpp-static ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  
  protobuf_generate(
    TARGET pinpoint_cpp-static
    LANGUAGE cpp
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR} ${grpc_SOURCE_DIR}/third_party/protobuf/src
  )

  protobuf_generate(
    TARGET pinpoint_cpp-static
    LANGUAGE grpc
    PLUGIN protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN_OPTIONS generate_mock_code=true
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR} ${grpc_SOURCE_DIR}/third_party/protobuf/src
  )
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

