# Performance numbers from unoptimized Debug executables are misleading.
if(MSVC)
  add_compile_options(/O2)
else()
  add_compile_options(-O3)
endif()
add_compile_definitions(NDEBUG)

add_executable(span_queue_benchmark span_queue_benchmark.cpp)
target_include_directories(span_queue_benchmark PRIVATE ../src)
target_link_libraries(span_queue_benchmark PRIVATE Threads::Threads)

# Header-only usage of src/cache.h: needs the public include dir for the
# pinpoint/tracer.h that utility.h pulls in, but no library to link.
add_executable(id_cache_benchmark id_cache_benchmark.cpp)
target_include_directories(id_cache_benchmark PRIVATE ../src ../include)
target_link_libraries(id_cache_benchmark PRIVATE Threads::Threads)

add_executable(atomic_shared_ptr_benchmark atomic_shared_ptr_benchmark.cpp)
target_include_directories(atomic_shared_ptr_benchmark PRIVATE ../src)
target_link_libraries(atomic_shared_ptr_benchmark PRIVATE Threads::Threads)

add_executable(raw_sql_cache_benchmark raw_sql_cache_benchmark.cpp)
target_include_directories(raw_sql_cache_benchmark PRIVATE ../src)
target_link_libraries(raw_sql_cache_benchmark PRIVATE
  ${PINPOINT_CPP_LIBRARY}
  Threads::Threads)

# Header-only usage of src/active_span.h: compares the former sharded-map
# active-span registry with the intrusive-list ActiveSpanRegistry.
add_executable(active_span_benchmark active_span_benchmark.cpp)
target_include_directories(active_span_benchmark PRIVATE ../src)
target_link_libraries(active_span_benchmark PRIVATE Threads::Threads)
