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
# MySQL Connector C++ (required)
# =============================================================================
find_path(MYSQLCPPCONN_INCLUDE_DIR mysqlx/xdevapi.h
    PATHS /usr/local/include /opt/homebrew/include /usr/include
    PATH_SUFFIXES mysql-connector-c++
)

find_library(MYSQLCPPCONNX_LIBRARY mysqlcppconnx
    PATHS /usr/local/lib /opt/homebrew/lib /usr/lib /usr/local/lib64
    PATH_SUFFIXES mysql-connector-c++
)

if(MYSQLCPPCONN_INCLUDE_DIR AND MYSQLCPPCONNX_LIBRARY)
    message(STATUS "MySQL Connector C++ found for it_test")
    message(STATUS "  Include: ${MYSQLCPPCONN_INCLUDE_DIR}")
    message(STATUS "  Library: ${MYSQLCPPCONNX_LIBRARY}")
else()
    message(FATAL_ERROR
        "MySQL Connector C++ is required for it_test but was not found.\n"
        "  macOS:   brew install mysql-connector-c++\n"
        "  Ubuntu:  apt-get install libmysqlcppconn-dev\n"
    )
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
    ${MYSQLCPPCONN_INCLUDE_DIR}
)
target_link_libraries(it_test_server
    ${PINPOINT_CPP_LIBRARY}
    it_test_proto
    ${MYSQLCPPCONNX_LIBRARY}
)
set_target_properties(it_test_server PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
