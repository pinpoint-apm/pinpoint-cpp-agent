find_package(Threads REQUIRED)

add_executable(span_queue_benchmark span_queue_benchmark.cpp)
target_include_directories(span_queue_benchmark PRIVATE ../src)
target_link_libraries(span_queue_benchmark PRIVATE Threads::Threads)
set_target_properties(span_queue_benchmark PROPERTIES CXX_STANDARD 17)

# Performance numbers from an unoptimized Debug executable are misleading.
# Keep this standalone target optimized even when it is built from the
# debug-cached preset used for the rest of the agent.
if(MSVC)
  target_compile_options(span_queue_benchmark PRIVATE /O2)
else()
  target_compile_options(span_queue_benchmark PRIVATE -O3)
endif()
target_compile_definitions(span_queue_benchmark PRIVATE NDEBUG)

if(TARGET pinpoint_cpp)
  set(PINPOINT_BENCHMARK_LIBRARY pinpoint_cpp)
elseif(TARGET pinpoint_cpp-static)
  set(PINPOINT_BENCHMARK_LIBRARY pinpoint_cpp-static)
endif()

if(PINPOINT_BENCHMARK_LIBRARY)
  add_executable(raw_sql_cache_benchmark raw_sql_cache_benchmark.cpp)
  target_include_directories(raw_sql_cache_benchmark PRIVATE ../src)
  target_link_libraries(raw_sql_cache_benchmark PRIVATE
    ${PINPOINT_BENCHMARK_LIBRARY}
    Threads::Threads)
  set_target_properties(raw_sql_cache_benchmark PROPERTIES CXX_STANDARD 17)
  if(MSVC)
    target_compile_options(raw_sql_cache_benchmark PRIVATE /O2)
  else()
    target_compile_options(raw_sql_cache_benchmark PRIVATE -O3)
  endif()
  target_compile_definitions(raw_sql_cache_benchmark PRIVATE NDEBUG)
endif()
