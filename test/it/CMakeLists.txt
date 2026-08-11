add_library(pinpoint_mock_collector STATIC
    mock_collector.cpp
    mock_collector.h
)
target_include_directories(pinpoint_mock_collector PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(pinpoint_mock_collector PUBLIC
    ${PINPOINT_CPP_LIBRARY}
    gRPC::grpc++
    protobuf::libprotobuf
    Threads::Threads
)
add_executable(agent_integration_test test_agent_integration.cpp)
target_include_directories(agent_integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(agent_integration_test PRIVATE
    pinpoint_mock_collector
    GTest::gtest_main
)
if(SANITIZE)
    target_compile_definitions(pinpoint_mock_collector PRIVATE PINPOINT_SANITIZER_TIMEOUT_SCALE=3)
    target_compile_definitions(agent_integration_test PRIVATE PINPOINT_SANITIZER_TIMEOUT_SCALE=3)
endif()
add_test(NAME agent_integration_test COMMAND agent_integration_test)
set_property(GLOBAL APPEND PROPERTY PINPOINT_TEST_TARGETS agent_integration_test)
set(_pinpoint_agent_integration_timeout 90)
if(SANITIZE)
    # Fully instrumented static gRPC/Protobuf dependencies are substantially
    # slower in resource-constrained CI containers.
    set(_pinpoint_agent_integration_timeout 900)
endif()
set_tests_properties(agent_integration_test PROPERTIES
    LABELS "integration;mock-collector"
    TIMEOUT ${_pinpoint_agent_integration_timeout}
)
unset(_pinpoint_agent_integration_timeout)
