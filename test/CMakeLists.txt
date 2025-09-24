if (BUILD_SHARED_LIBS)
  set(PINPOINT_CPP_LIBRARY pinpoint_cpp)
else()
  set(PINPOINT_CPP_LIBRARY pinpoint_cpp-static)
endif()

# Find GoogleTest
find_package(GTest CONFIG REQUIRED)

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

#add_executable(pinpoint_tracer_test tracer_test.cpp)
#target_link_libraries(pinpoint_tracer_test ${PINPOINT_CPP_LIBRARY})
#add_test(NAME pinpoint_tracer_test COMMAND pinpoint_tracer_test)
