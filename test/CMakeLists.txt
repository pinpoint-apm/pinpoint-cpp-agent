include(FetchContent)

find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
  message(STATUS "GoogleTest not found on system, fetching from source...")
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
else()
  message(STATUS "Using system GoogleTest")
endif()

function(add_pinpoint_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ${ARGN})
  add_test(NAME ${name} COMMAND ${name})
  # The coverage target builds every registered test target via this list.
  set_property(GLOBAL APPEND PROPERTY PINPOINT_TEST_TARGETS ${name})
endfunction()

add_pinpoint_test(test_atomic_shared_ptr
  GTest::gtest GTest::gtest_main Threads::Threads)
add_pinpoint_test(test_sharded_bounded_queue
  GTest::gtest GTest::gtest_main)

foreach(name IN ITEMS
    test_limiter
    test_sampling
    test_cache
    test_http
    test_annotation
    test_callstack
    test_config
    test_sql
    test_stat
    test_url_stat
    test_span_event
    test_span
    test_noop
    test_utility
    test_logging
    test_object_name)
  add_pinpoint_test(${name}
    ${PINPOINT_CPP_LIBRARY} GTest::gtest GTest::gtest_main)
endforeach()

foreach(name IN ITEMS
    test_grpc
    test_grpc_with_mocks
    test_agent_with_mocks
    test_fork
    test_tracer_c)
  add_pinpoint_test(${name} ${PINPOINT_CPP_LIBRARY} GTest::gmock_main)
endforeach()

add_subdirectory(it)
add_subdirectory(e2e)
