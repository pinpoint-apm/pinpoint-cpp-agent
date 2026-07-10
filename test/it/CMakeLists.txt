if(BUILD_SHARED_LIBS)
    set(PINPOINT_CPP_IT_LIBRARY pinpoint_cpp)
else()
    set(PINPOINT_CPP_IT_LIBRARY pinpoint_cpp-static)
endif()

find_package(Threads REQUIRED)

add_library(pinpoint_mock_collector STATIC
    mock_collector.cpp
    mock_collector.h
)
target_include_directories(pinpoint_mock_collector PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(pinpoint_mock_collector PUBLIC
    ${PINPOINT_CPP_IT_LIBRARY}
    gRPC::grpc++
    protobuf::libprotobuf
    Threads::Threads
)
set_target_properties(pinpoint_mock_collector PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
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
set_target_properties(agent_integration_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
)

add_test(NAME agent_integration_test COMMAND agent_integration_test)
set_tests_properties(agent_integration_test PROPERTIES
    LABELS "integration;mock-collector"
    TIMEOUT 90
)
