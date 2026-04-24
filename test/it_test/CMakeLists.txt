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

target_link_libraries(grpc_server ${PINPOINT_CPP_LIBRARY} it_test_proto gRPC::grpc++_reflection)
set_target_properties(grpc_server PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)

# =============================================================================
# it_test_server (HTTP + gRPC client + SQL tracing)
# =============================================================================
add_executable(it_test_server it_test_server.cpp)
target_include_directories(it_test_server PRIVATE
    ${CMAKE_SOURCE_DIR}/example
    ${CMAKE_SOURCE_DIR}/3rd_party
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(it_test_server
    ${PINPOINT_CPP_LIBRARY}
    it_test_proto
)
set_target_properties(it_test_server PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
