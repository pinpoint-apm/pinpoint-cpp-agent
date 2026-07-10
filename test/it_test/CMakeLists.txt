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
# HTTP downstream server (real distributed HTTP context propagation)
# =============================================================================
add_executable(http_downstream_server http_downstream_server.cpp)
target_include_directories(http_downstream_server PRIVATE
    ${CMAKE_SOURCE_DIR}/example
    ${CMAKE_SOURCE_DIR}/3rd_party
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(http_downstream_server ${PINPOINT_CPP_LIBRARY})
set_target_properties(http_downstream_server PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)

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

# =============================================================================
# Standalone public API/lifecycle scenarios
# =============================================================================
find_package(Threads REQUIRED)

add_executable(c_api_scenario c_api_scenario.c)
target_link_libraries(c_api_scenario ${PINPOINT_CPP_LIBRARY} Threads::Threads)
set_target_properties(c_api_scenario PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    LINKER_LANGUAGE CXX
)

if(UNIX)
    add_executable(fork_scenario fork_scenario.cpp)
    target_include_directories(fork_scenario PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(fork_scenario ${PINPOINT_CPP_LIBRARY} Threads::Threads)
    set_target_properties(fork_scenario PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
endif()

# The live suite depends on the configured dev collector and is intentionally
# opt-in for CTest. It remains directly runnable through this custom target.
find_program(BASH_PROGRAM bash)
if(BASH_PROGRAM AND UNIX)
    set(IT_TEST_DEPENDS
        grpc_server
        http_downstream_server
        it_test_server
        c_api_scenario
        fork_scenario
    )
    add_custom_target(run_it_test
        COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/run_it_test.sh
                --build-dir $<TARGET_FILE_DIR:it_test_server>
        DEPENDS ${IT_TEST_DEPENDS}
        USES_TERMINAL
        VERBATIM
    )

    option(PINPOINT_REGISTER_LIVE_IT_TEST
           "Register the dev-collector integration suite with CTest" OFF)
    if(PINPOINT_REGISTER_LIVE_IT_TEST)
        add_test(
            NAME live_integration_test
            COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/run_it_test.sh
                    --build-dir $<TARGET_FILE_DIR:it_test_server>
        )
        set_tests_properties(live_integration_test PROPERTIES
            LABELS "integration;live;requires-network"
            RUN_SERIAL TRUE
            TIMEOUT 300
        )
    endif()
endif()
