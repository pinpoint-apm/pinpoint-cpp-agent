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
