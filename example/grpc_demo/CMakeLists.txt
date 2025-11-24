set(GRPC_DEMO_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/testapp.proto)

add_library(grpc_demo_proto STATIC)

protobuf_generate(
    TARGET grpc_demo_proto
    LANGUAGE cpp
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
    PROTOS ${GRPC_DEMO_PROTO}
)

protobuf_generate(
    TARGET grpc_demo_proto
    LANGUAGE grpc
    PLUGIN protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
    PROTOS ${GRPC_DEMO_PROTO}
)

target_include_directories(grpc_demo_proto PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(grpc_demo_proto PUBLIC grpc++ protobuf::libprotobuf)
set_target_properties(grpc_demo_proto PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)

add_executable(grpc_server grpc_server.cpp)
target_include_directories(grpc_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(grpc_server
    ${PINPOINT_CPP_LIBRARY}
    grpc_demo_proto
    grpc++_reflection
)
set_target_properties(grpc_server PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)

add_executable(grpc_client grpc_client.cpp)
target_include_directories(grpc_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(grpc_client
    ${PINPOINT_CPP_LIBRARY}
    grpc_demo_proto
)
set_target_properties(grpc_client PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)

