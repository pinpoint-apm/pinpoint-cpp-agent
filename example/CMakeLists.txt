if (BUILD_SHARED_LIBS)
    set(PINPOINT_CPP_LIBRARY pinpoint_cpp)
else()
    set(PINPOINT_CPP_LIBRARY pinpoint_cpp-static)
endif()

add_executable(tutorial tutorial.cpp)
target_link_libraries(tutorial ${PINPOINT_CPP_LIBRARY})

add_executable(http_server http_server.cpp)
target_link_libraries(http_server ${PINPOINT_CPP_LIBRARY})

# C-only ports using the pure-C tracer API (tracer_c.h) and a thin C++
# wrapper around cpp-httplib (httplib_c.h / httplib_c.cpp).
add_executable(http_server_c http_server_c.c httplib_c.cpp)
target_link_libraries(http_server_c ${PINPOINT_CPP_LIBRARY})
set_source_files_properties(http_server_c.c PROPERTIES LANGUAGE C)
set_target_properties(http_server_c PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

add_executable(tutorial_c tutorial_c.c httplib_c.cpp)
target_link_libraries(tutorial_c ${PINPOINT_CPP_LIBRARY})
set_source_files_properties(tutorial_c.c PROPERTIES LANGUAGE C)
set_target_properties(tutorial_c PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)
