# =============================================================================
# Pinpoint C Wrapper - CivetWeb Example
# =============================================================================
# This example demonstrates how to integrate Pinpoint distributed tracing
# with a CivetWeb HTTP server using the Pinpoint C wrapper API.
# =============================================================================

#cmake_minimum_required(VERSION 3.10)
#project(pinpoint_civetweb_example C)

# Find or download CivetWeb
find_package(civetweb CONFIG)

if(NOT civetweb_FOUND)
    message(STATUS "CivetWeb not found, will try pkg-config")
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(CIVETWEB civetweb)
    endif()
endif()

if(NOT civetweb_FOUND AND NOT CIVETWEB_FOUND)
    message(STATUS "CivetWeb not found via CMake or pkg-config")
    message(STATUS "Attempting to use system-installed CivetWeb library")
    
    # Try to find the library manually
    find_path(CIVETWEB_INCLUDE_DIR civetweb.h
        PATHS
        /usr/include
        /usr/local/include
        /opt/homebrew/include
    )
    
    find_library(CIVETWEB_LIBRARY
        NAMES civetweb civetweb-cpp libcivetweb
        PATHS
        /usr/lib
        /usr/local/lib
        /opt/homebrew/lib
    )
    
    if(CIVETWEB_INCLUDE_DIR AND CIVETWEB_LIBRARY)
        message(STATUS "Found CivetWeb manually:")
        message(STATUS "  Include: ${CIVETWEB_INCLUDE_DIR}")
        message(STATUS "  Library: ${CIVETWEB_LIBRARY}")
        set(CIVETWEB_FOUND TRUE)
    else()
        message(WARNING "CivetWeb not found. Please install CivetWeb:")
        message(WARNING "  Ubuntu/Debian: sudo apt-get install libcivetweb-dev")
        message(WARNING "  macOS: brew install civetweb")
        message(WARNING "  Or build from source: https://github.com/civetweb/civetweb")
        return()
    endif()
endif()

# Create executable
add_executable(civetweb_example civetweb_example.c)

# Include directories
target_include_directories(civetweb_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if(CIVETWEB_INCLUDE_DIR)
    target_include_directories(civetweb_example PRIVATE ${CIVETWEB_INCLUDE_DIR})
endif()

# Link libraries
if(TARGET pinpoint_c)
    target_link_libraries(civetweb_example PRIVATE pinpoint_c)
elseif(TARGET pinpoint_c-static)
    target_link_libraries(civetweb_example PRIVATE pinpoint_c-static)
else()
    # Try to find the installed library
    find_library(PINPOINT_C_LIB pinpoint_c PATHS ${CMAKE_BINARY_DIR}/export)
    if(PINPOINT_C_LIB)
        target_link_libraries(civetweb_example PRIVATE ${PINPOINT_C_LIB})
    else()
        message(WARNING "Pinpoint C library not found")
    endif()
endif()

if(TARGET civetweb::civetweb)
    target_link_libraries(civetweb_example PRIVATE civetweb::civetweb)
elseif(TARGET civetweb::civetweb-cpp)
    target_link_libraries(civetweb_example PRIVATE civetweb::civetweb-cpp)
elseif(CIVETWEB_LIBRARIES)
    target_link_libraries(civetweb_example PRIVATE ${CIVETWEB_LIBRARIES})
elseif(CIVETWEB_LIBRARY)
    target_link_libraries(civetweb_example PRIVATE ${CIVETWEB_LIBRARY})
endif()

# Also link the Pinpoint C++ library if available
if(TARGET pinpoint_cpp)
    target_link_libraries(civetweb_example PRIVATE pinpoint_cpp)
elseif(TARGET pinpoint_cpp-static)
    target_link_libraries(civetweb_example PRIVATE pinpoint_cpp-static)
endif()

# Platform-specific settings
if(WIN32)
    target_link_libraries(civetweb_example PRIVATE ws2_32)
else()
    target_link_libraries(civetweb_example PRIVATE pthread)
endif()

# Set C standard
set_target_properties(civetweb_example PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)

# Install
install(TARGETS civetweb_example
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Copy example config file to build directory
if(EXISTS ${CMAKE_SOURCE_DIR}/example/http_server.yaml)
    configure_file(
        ${CMAKE_SOURCE_DIR}/example/http_server.yaml
        ${CMAKE_CURRENT_BINARY_DIR}/pinpoint-config.yaml
        COPYONLY
    )
endif()

message(STATUS "CivetWeb example configured")

