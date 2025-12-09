if (BUILD_SHARED_LIBS)
    set(PINPOINT_CPP_LIBRARY pinpoint_cpp)
else()
    set(PINPOINT_CPP_LIBRARY pinpoint_cpp-static)
endif()

include(FetchContent)

# Try to find GoogleTest on the system first
find_package(GTest CONFIG QUIET)

if(NOT GTest_FOUND)
  message(STATUS "GoogleTest not found on system, fetching from source...")
  
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
  )
  # For Windows: Prevent overriding the parent project's compiler/linker settings
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
else()
  message(STATUS "Using system GoogleTest")
endif()

# RateLimiter tests
add_executable(test_limiter test_limiter.cpp)
target_include_directories(test_limiter PRIVATE ../src)
target_link_libraries(test_limiter 
    ${PINPOINT_CPP_LIBRARY} 
    GTest::gtest 
    GTest::gtest_main
)
set_target_properties(test_limiter PROPERTIES CXX_STANDARD 17)
add_test(NAME test_limiter COMMAND test_limiter)

# Sampling tests
add_executable(test_sampling test_sampling.cpp)
target_include_directories(test_sampling PRIVATE ../src)
target_link_libraries(test_sampling 
    ${PINPOINT_CPP_LIBRARY} 
    GTest::gtest 
    GTest::gtest_main
)
set_target_properties(test_sampling PROPERTIES CXX_STANDARD 17)
add_test(NAME test_sampling COMMAND test_sampling)

# Cache tests
add_executable(test_cache test_cache.cpp)
target_include_directories(test_cache PRIVATE ../src)
target_link_libraries(test_cache 
    ${PINPOINT_CPP_LIBRARY} 
    GTest::gtest 
    GTest::gtest_main
)
set_target_properties(test_cache PROPERTIES CXX_STANDARD 17)
add_test(NAME test_cache COMMAND test_cache)

# HTTP tests
add_executable(test_http test_http.cpp)
target_include_directories(test_http PRIVATE ../src)
target_link_libraries(test_http 
    ${PINPOINT_CPP_LIBRARY} 
    GTest::gtest 
    GTest::gtest_main
)
set_target_properties(test_http PROPERTIES CXX_STANDARD 17)
add_test(NAME test_http COMMAND test_http)

# Annotation tests
add_executable(test_annotation test_annotation.cpp)
target_include_directories(test_annotation PRIVATE ../src)
target_link_libraries(test_annotation 
    ${PINPOINT_CPP_LIBRARY} 
    GTest::gtest 
    GTest::gtest_main
)
set_target_properties(test_annotation PROPERTIES CXX_STANDARD 17)
add_test(NAME test_annotation COMMAND test_annotation)

# CallStack tests
add_executable(test_callstack test_callstack.cpp)
target_include_directories(test_callstack PRIVATE ../src)
target_link_libraries(test_callstack 
    ${PINPOINT_CPP_LIBRARY} 
    GTest::gtest 
    GTest::gtest_main
)
set_target_properties(test_callstack PROPERTIES CXX_STANDARD 17)
add_test(NAME test_callstack COMMAND test_callstack)

# Config tests
add_executable(test_config test_config.cpp)
target_include_directories(test_config PRIVATE ../src)
target_link_libraries(test_config 
    ${PINPOINT_CPP_LIBRARY} 
    GTest::gtest 
    GTest::gtest_main
)
set_target_properties(test_config PROPERTIES CXX_STANDARD 17)
add_test(NAME test_config COMMAND test_config)

# SQL tests
add_executable(test_sql test_sql.cpp)
target_include_directories(test_sql PRIVATE ../src)
target_link_libraries(test_sql 
    ${PINPOINT_CPP_LIBRARY} 
    GTest::gtest 
    GTest::gtest_main
)
set_target_properties(test_sql PROPERTIES CXX_STANDARD 17)
add_test(NAME test_sql COMMAND test_sql)

# Add test_stat
add_executable(test_stat test_stat.cpp)
target_link_libraries(test_stat 
    ${PINPOINT_CPP_LIBRARY} 
    GTest::gtest 
    GTest::gtest_main
)
set_target_properties(test_stat PROPERTIES CXX_STANDARD 17)
add_test(NAME test_stat COMMAND test_stat)

# Add test_url_stat
add_executable(test_url_stat test_url_stat.cpp)
target_link_libraries(test_url_stat 
    ${PINPOINT_CPP_LIBRARY} 
    GTest::gtest 
    GTest::gtest_main
)
set_target_properties(test_url_stat PROPERTIES CXX_STANDARD 17)
add_test(NAME test_url_stat COMMAND test_url_stat)

# Add test_span_event
add_executable(test_span_event test_span_event.cpp)
target_link_libraries(test_span_event 
    ${PINPOINT_CPP_LIBRARY} 
    GTest::gtest 
    GTest::gtest_main
)
set_target_properties(test_span_event PROPERTIES CXX_STANDARD 17)
add_test(NAME test_span_event COMMAND test_span_event)

# Add test_span
add_executable(test_span test_span.cpp)
target_link_libraries(test_span 
    ${PINPOINT_CPP_LIBRARY} 
    GTest::gtest 
    GTest::gtest_main
)
set_target_properties(test_span PROPERTIES CXX_STANDARD 17)
add_test(NAME test_span COMMAND test_span)

# Add test_noop
add_executable(test_noop test_noop.cpp)
target_link_libraries(test_noop 
    ${PINPOINT_CPP_LIBRARY} 
    GTest::gtest 
    GTest::gtest_main
)
set_target_properties(test_noop PROPERTIES CXX_STANDARD 17)
add_test(NAME test_noop COMMAND test_noop)

# Add test_grpc
add_executable(test_grpc test_grpc.cpp)
target_link_libraries(test_grpc 
    ${PINPOINT_CPP_LIBRARY} 
    GTest::gmock_main
)
set_target_properties(test_grpc PROPERTIES CXX_STANDARD 17)
add_test(NAME test_grpc COMMAND test_grpc)

# Add test_grpc_with_mocks
add_executable(test_grpc_with_mocks test_grpc_with_mocks.cpp)
target_link_libraries(test_grpc_with_mocks 
    ${PINPOINT_CPP_LIBRARY} 
    GTest::gmock_main
)
set_target_properties(test_grpc_with_mocks PROPERTIES CXX_STANDARD 17)
add_test(NAME test_grpc_with_mocks COMMAND test_grpc_with_mocks)
