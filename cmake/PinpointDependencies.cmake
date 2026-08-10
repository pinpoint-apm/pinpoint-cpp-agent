include_guard(GLOBAL)

include(FetchContent)

# CMake dependency resolution deliberately supports exactly two modes:
#   1. packages supplied by the active vcpkg toolchain;
#   2. pinned sources built with FetchContent.
# Do not probe CMAKE_PREFIX_PATH or the host system for these libraries.
set(PINPOINT_VCPKG_TOOLCHAIN_ACTIVE OFF)
if(VCPKG_TOOLCHAIN)
  set(PINPOINT_VCPKG_TOOLCHAIN_ACTIVE ON)
endif()

# Limit both the requested config package and any find_dependency() calls made
# by that config to the active vcpkg installation. This prevents a partially
# populated vcpkg tree from silently filling gaps with Homebrew/apt packages.
function(pinpoint_find_vcpkg_package package_name)
  if(NOT VCPKG_INSTALLED_DIR OR NOT VCPKG_TARGET_TRIPLET)
    message(FATAL_ERROR
      "The active vcpkg toolchain did not define VCPKG_INSTALLED_DIR and "
      "VCPKG_TARGET_TRIPLET.")
  endif()

  set(_pp_vcpkg_prefix
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
  string(TOLOWER "${package_name}" _pp_vcpkg_package_dir)
  set(${package_name}_DIR
    "${_pp_vcpkg_prefix}/share/${_pp_vcpkg_package_dir}")
  set(CMAKE_PREFIX_PATH "${_pp_vcpkg_prefix}")
  set(CMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH OFF)
  set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF)
  set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH OFF)
  set(CMAKE_FIND_USE_INSTALL_PREFIX OFF)
  set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
  set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF)

  find_package(${package_name} CONFIG REQUIRED
    PATHS "${_pp_vcpkg_prefix}"
    NO_DEFAULT_PATH)
endfunction()

if(PINPOINT_VCPKG_TOOLCHAIN_ACTIVE AND NOT PINPOINT_FORCE_FETCHCONTENT)
  set(PINPOINT_DEPENDENCY_PROVIDER "vcpkg")
  message(STATUS "Pinpoint dependency provider: vcpkg toolchain")

  pinpoint_find_vcpkg_package(Protobuf)
  pinpoint_find_vcpkg_package(absl)
  pinpoint_find_vcpkg_package(yaml-cpp)
  pinpoint_find_vcpkg_package(fmt)
  pinpoint_find_vcpkg_package(gRPC)

  # vcpkg's protobuf port installs the generation helper next to its config.
  # Avoid FindProtobuf here because it can fall through to a system package.
  if(NOT COMMAND protobuf_generate)
    include("${Protobuf_DIR}/../protobuf-generate.cmake" OPTIONAL)
  endif()
  if(NOT COMMAND protobuf_generate)
    message(FATAL_ERROR
      "The vcpkg Protobuf package did not provide protobuf_generate().")
  endif()
else()
  set(PINPOINT_DEPENDENCY_PROVIDER "fetchcontent")
  if(PINPOINT_VCPKG_TOOLCHAIN_ACTIVE)
    message(STATUS
      "Pinpoint dependency provider: FetchContent (source build forced)")
  else()
    message(STATUS
      "Pinpoint dependency provider: FetchContent (no vcpkg toolchain)")
  endif()

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
  set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(CARES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(CARES_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
  set(utf8_range_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(gRPC)

  # Match the target names exported by the vcpkg packages.
  foreach(_pp_grpc_target IN ITEMS grpc++ grpc++_reflection)
    if(TARGET ${_pp_grpc_target} AND NOT TARGET gRPC::${_pp_grpc_target})
      add_library(gRPC::${_pp_grpc_target} ALIAS ${_pp_grpc_target})
    endif()
  endforeach()
  if(TARGET grpc_cpp_plugin AND NOT TARGET gRPC::grpc_cpp_plugin)
    add_executable(gRPC::grpc_cpp_plugin ALIAS grpc_cpp_plugin)
  endif()

  include("${grpc_SOURCE_DIR}/third_party/protobuf/cmake/protobuf-generate.cmake")

  FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0
  )
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(yaml-cpp)

  FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        11.2.0
  )
  FetchContent_MakeAvailable(fmt)
endif()
