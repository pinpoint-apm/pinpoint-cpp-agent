cmake_minimum_required(VERSION 3.20)

set(CMAKE_VERBOSE_MAKEFILE on)

# =============================================================================
# vcpkg Auto-Detection
# =============================================================================
# Detect and configure vcpkg if available on the system.
# Priority order:
#   1. CMAKE_TOOLCHAIN_FILE already set (user explicitly provided)
#   2. VCPKG_ROOT environment variable
#   3. Common vcpkg installation paths
# If vcpkg is not found, FetchContent will be used as fallback.
# =============================================================================

# Check VCPKG_ROOT environment variable first
if(DEFINED ENV{VCPKG_ROOT})
  set(VCPKG_ROOT "$ENV{VCPKG_ROOT}")
  message(STATUS "Found VCPKG_ROOT environment variable: ${VCPKG_ROOT}")
endif()
  
# If VCPKG_ROOT is found, set the toolchain file
if(DEFINED VCPKG_ROOT AND EXISTS "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
  set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      CACHE FILEPATH "Vcpkg toolchain file")
  set(VCPKG_DETECTED TRUE CACHE BOOL "vcpkg was auto-detected")
  message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
else()
  set(VCPKG_DETECTED FALSE CACHE BOOL "vcpkg was not found")
  message(STATUS "vcpkg not found, will use FetchContent for dependencies")
endif()

include(CMakeDependentOption)

project(pinpoint-cpp VERSION 1.0.0)

# Option to force FetchContent even when vcpkg is available
option(FORCE_FETCHCONTENT "Force using FetchContent instead of vcpkg" OFF)

if(FORCE_FETCHCONTENT)
  set(VCPKG_DETECTED FALSE)
  message(STATUS "FORCE_FETCHCONTENT is ON, will use FetchContent for dependencies")
endif()

option(BUILD_TESTING "Build tests" ON)
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_SHARED_LIBS "Build as a shared library" OFF)
option(BUILD_STATIC_LIBS "Build as a static library" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Dependency Resolution Strategy
# =============================================================================
# If vcpkg is detected and FORCE_FETCHCONTENT is OFF:
#   - Dependencies will be resolved via vcpkg (find_package should succeed)
# Otherwise:
#   - Try find_package first, then fallback to FetchContent
# =============================================================================

message(STATUS "=== Dependency Resolution ===")
if(VCPKG_DETECTED)
  message(STATUS "Strategy: Using vcpkg for package management")
else()
  message(STATUS "Strategy: FetchContent fallback (vcpkg not available)")
  include(FetchContent)
endif()
message(STATUS "=============================")

# Try to find gRPC installed on the system first
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

if(NOT gRPC_FOUND OR NOT Protobuf_FOUND)
  message(STATUS "gRPC not found on system, fetching from source...")
  
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
  set(utf8_range_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(gRPC)
  
  # Include protobuf-generate module for protobuf_generate() function
  include(${grpc_SOURCE_DIR}/third_party/protobuf/cmake/protobuf-generate.cmake)
else()
  message(STATUS "Using system gRPC and Protobuf")
  
  # Find absl (required when using system gRPC)
  find_package(absl CONFIG REQUIRED)
  
  # Include protobuf-generate module from system installation
  include(${Protobuf_DIR}/../protobuf-generate.cmake OPTIONAL)
  if(NOT COMMAND protobuf_generate)
    # Fallback: try to include from CMake modules
    include(FindProtobuf)
  endif()
endif()

list(APPEND LINK_LIBS protobuf::libprotobuf)
if(VCPKG_DETECTED)
  list(APPEND LINK_LIBS gRPC::grpc gRPC::grpc++ gRPC::grpc++_reflection)
  list(APPEND LINK_LIBS gRPC::gpr gRPC::grpc_unsecure gRPC::grpc++_unsecure)
  list(APPEND LINK_LIBS gRPC::grpc++_error_details gRPC::grpcpp_channelz)
else()
  list(APPEND LINK_LIBS grpc grpc++ grpc++_reflection)
  list(APPEND LINK_LIBS gpr grpc_unsecure grpc++_unsecure)
  list(APPEND LINK_LIBS grpc++_error_details grpcpp_channelz)
endif()
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

# Try to find yaml-cpp on the system first
find_package(yaml-cpp CONFIG REQUIRED)

if(NOT yaml-cpp_FOUND)
  message(STATUS "yaml-cpp not found on system, fetching from source...")
  
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
else()
  message(STATUS "Using system yaml-cpp")
endif()

list(APPEND LINK_LIBS yaml-cpp::yaml-cpp)

# Try to find fmt on the system first
find_package(fmt CONFIG REQUIRED)

if(NOT fmt_FOUND)
  message(STATUS "fmt not found on system, fetching from source...")
  
  # Fetch fmt
  FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        11.0.2
  )
  FetchContent_MakeAvailable(fmt)
else()
  message(STATUS "Using system fmt")
endif()

list(APPEND LINK_LIBS fmt::fmt)

# Try to find spdlog on the system first
find_package(spdlog CONFIG REQUIRED)

if(NOT spdlog_FOUND)
  message(STATUS "spdlog not found on system, fetching from source...")
  
  # Fetch spdlog
  FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
  )
  FetchContent_MakeAvailable(spdlog)
else()
  message(STATUS "Using system spdlog")
endif()

list(APPEND LINK_LIBS spdlog::spdlog)

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
         3rd_party/MurmurHash3.cpp)

if (BUILD_SHARED_LIBS)
  add_library(pinpoint_cpp SHARED ${SRCS} ${PROTOS})
  target_include_directories(pinpoint_cpp INTERFACE "$<INSTALL_INTERFACE:include/>")
  set_target_properties(pinpoint_cpp PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
  target_link_libraries(pinpoint_cpp PUBLIC ${LINK_LIBS})
  install(TARGETS pinpoint_cpp LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

  if(VCPKG_DETECTED)
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
  else()
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
  
  if(VCPKG_DETECTED)
    protobuf_generate(
      TARGET pinpoint_cpp-static
      LANGUAGE cpp
    )
    
    protobuf_generate(
      TARGET pinpoint_cpp-static
      LANGUAGE grpc
      PLUGIN protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
      GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
      PLUGIN_OPTIONS generate_mock_code=true
    )
  else()
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
