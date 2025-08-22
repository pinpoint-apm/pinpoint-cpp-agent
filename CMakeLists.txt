cmake_minimum_required(VERSION 3.20)

set(CMAKE_VERBOSE_MAKEFILE on)
set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH
        "Toolchain to use for building this package and dependencies")

include(CMakeDependentOption)

project(pinpoint-cpp VERSION 1.0.0)

option(BUILD_TESTING "Build tests" OFF)
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_SHARED_LIBS "Build as a shared library" ON)
option(BUILD_STATIC_LIBS "Build as a static library" ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Protobuf CONFIG REQUIRED)
list(APPEND LINK_LIBS protobuf::libprotobuf)

find_package(gRPC CONFIG REQUIRED)
list(APPEND LINK_LIBS gRPC::grpc gRPC::grpc++ gRPC::grpc++_reflection)
list(APPEND LINK_LIBS gRPC::gpr gRPC::grpc_unsecure gRPC::grpc++_unsecure)
list(APPEND LINK_LIBS gRPC::grpc++_error_details gRPC::grpcpp_channelz)

find_package(yaml-cpp CONFIG REQUIRED)
list(APPEND LINK_LIBS yaml-cpp::yaml-cpp)

find_package(fmt CONFIG REQUIRED)
list(APPEND LINK_LIBS fmt::fmt)

find_package(absl CONFIG REQUIRED)
list(APPEND LINK_LIBS absl::strings)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
         src/utility.cpp)

if (BUILD_SHARED_LIBS)
  add_library(pinpoint_cpp SHARED ${SRCS} ${PROTOS})
  target_include_directories(pinpoint_cpp INTERFACE "$<INSTALL_INTERFACE:include/>")
  set_target_properties(pinpoint_cpp PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
  target_link_libraries(pinpoint_cpp PUBLIC ${LINK_LIBS})
  install(TARGETS pinpoint_cpp LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if (BUILD_STATIC_LIBS)
  add_library(pinpoint_cpp-static STATIC ${SRCS} ${PROTOS})
  set_target_properties(pinpoint_cpp-static PROPERTIES OUTPUT_NAME pinpoint_cpp)
  target_include_directories(pinpoint_cpp-static INTERFACE "$<INSTALL_INTERFACE:include/>")
  target_link_libraries(pinpoint_cpp-static ${LINK_LIBS})
  install(TARGETS pinpoint_cpp-static ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

protobuf_generate(
  TARGET pinpoint_cpp
  LANGUAGE cpp
)

protobuf_generate(
  TARGET pinpoint_cpp
  LANGUAGE grpc
  PLUGIN protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
)

install(DIRECTORY include/pinpoint DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Testing
if (BUILD_TESTING)
  include(CTest)
  add_subdirectory(test)
endif()

if (BUILD_EXAMPLES)
  add_subdirectory(example)
endif()

