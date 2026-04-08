if (BUILD_SHARED_LIBS)
    set(PINPOINT_CPP_LIBRARY pinpoint_cpp)
else()
    set(PINPOINT_CPP_LIBRARY pinpoint_cpp-static)
endif()

# =============================================================================
# gRPC proto library (testapp.proto)
# =============================================================================
set(IT_TEST_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/testapp.proto)

add_library(it_test_proto STATIC)

if(VCPKG_DETECTED)
    protobuf_generate(
        TARGET it_test_proto
        LANGUAGE cpp
        IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
        PROTOS ${IT_TEST_PROTO}
    )
    protobuf_generate(
        TARGET it_test_proto
        LANGUAGE grpc
        PLUGIN protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
        GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
        IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
        PROTOS ${IT_TEST_PROTO}
    )
    target_link_libraries(it_test_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)
else()
    protobuf_generate(
        TARGET it_test_proto
        LANGUAGE cpp
        IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
        PROTOS ${IT_TEST_PROTO}
    )
    protobuf_generate(
        TARGET it_test_proto
        LANGUAGE grpc
        PLUGIN protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>
        GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
        IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
        PROTOS ${IT_TEST_PROTO}
    )
    target_link_libraries(it_test_proto PUBLIC grpc++ protobuf::libprotobuf)
endif()

target_include_directories(it_test_proto PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(it_test_proto PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)

# =============================================================================
# gRPC server (standalone)
# =============================================================================
add_executable(grpc_server grpc_server.cpp)
target_include_directories(grpc_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(grpc_server ${PINPOINT_CPP_LIBRARY} it_test_proto)
set_target_properties(grpc_server PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)

# =============================================================================
# MySQL Connector C++ X DevAPI (required)
# =============================================================================
# Try CMake config first (available in MySQL APT repo packages 8.3.0+)
find_package(mysql-concpp QUIET)

if(mysql-concpp_FOUND)
    message(STATUS "MySQL Connector C++ found via CMake config (mysql-concpp)")
    set(MYSQL_CONNECTOR_TARGET mysql::concpp-xdevapi)
else()
    # Fallback: manual search
    # Header paths by platform:
    #   macOS (Homebrew):  /opt/homebrew/include/mysql-connector-c++/
    #   Ubuntu (MySQL APT repo 9.x):  /usr/include/mysql-cppconn/
    #   Ubuntu (MySQL APT repo 8.x):  /usr/include/mysql-cppconn-8/
    #   Manual install:    /usr/local/include/
    find_path(MYSQLCPPCONN_INCLUDE_DIR mysqlx/xdevapi.h
        PATHS /usr/local/include /opt/homebrew/include /usr/include
        PATH_SUFFIXES mysql-cppconn mysql-cppconn-8 mysql-connector-c++
    )

    find_library(MYSQLCPPCONNX_LIBRARY mysqlcppconnx
        PATHS /usr/local/lib /opt/homebrew/lib /usr/lib /usr/local/lib64
              /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu
        PATH_SUFFIXES mysql-connector-c++
    )

    if(MYSQLCPPCONN_INCLUDE_DIR AND MYSQLCPPCONNX_LIBRARY)
        message(STATUS "MySQL Connector C++ found (manual)")
        message(STATUS "  Include: ${MYSQLCPPCONN_INCLUDE_DIR}")
        message(STATUS "  Library: ${MYSQLCPPCONNX_LIBRARY}")
    else()
        message(FATAL_ERROR
            "MySQL Connector C++ (X DevAPI) is required for it_test but was not found.\n"
            "Install MySQL Connector C++ 8.x+ with X DevAPI support:\n"
            "  macOS:   brew install mysql-connector-c++\n"
            "  Ubuntu:  Install from MySQL official APT repository:\n"
            "           wget https://dev.mysql.com/get/mysql-apt-config_0.8.33-1_all.deb\n"
            "           sudo dpkg -i mysql-apt-config_0.8.33-1_all.deb\n"
            "           sudo apt-get update && sudo apt-get install libmysqlcppconn-dev\n"
            "  Manual:  Download tarball from https://dev.mysql.com/downloads/connector/cpp/\n"
            "           and set -DCMAKE_PREFIX_PATH=/path/to/extracted/dir\n"
            "\n"
            "NOTE: Ubuntu's default 'libmysqlcppconn-dev' (v1.1.x) does NOT include X DevAPI.\n"
            "      You must use the MySQL official APT repository or manual tarball install.\n"
        )
    endif()
endif()

# =============================================================================
# it_test_server (HTTP + gRPC client + MySQL)
# =============================================================================
add_executable(it_test_server it_test_server.cpp)
target_include_directories(it_test_server PRIVATE
    ${CMAKE_SOURCE_DIR}/example
    ${CMAKE_SOURCE_DIR}/3rd_party
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

if(mysql-concpp_FOUND)
    target_link_libraries(it_test_server
        ${PINPOINT_CPP_LIBRARY}
        it_test_proto
        ${MYSQL_CONNECTOR_TARGET}
    )
else()
    target_include_directories(it_test_server PRIVATE ${MYSQLCPPCONN_INCLUDE_DIR})
    target_link_libraries(it_test_server
        ${PINPOINT_CPP_LIBRARY}
        it_test_proto
        ${MYSQLCPPCONNX_LIBRARY}
    )
endif()
set_target_properties(it_test_server PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
