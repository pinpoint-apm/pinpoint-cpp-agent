if (BUILD_SHARED_LIBS)
    set(PINPOINT_CPP_LIBRARY pinpoint_cpp)
else()
    set(PINPOINT_CPP_LIBRARY pinpoint_cpp-static)
endif()

add_executable(tutorial tutorial.cpp)
target_link_libraries(tutorial ${PINPOINT_CPP_LIBRARY})

add_executable(http_server http_server.cpp)
target_link_libraries(http_server ${PINPOINT_CPP_LIBRARY})

add_executable(web_demo web_demo.cpp)
target_link_libraries(web_demo ${PINPOINT_CPP_LIBRARY})

#add_executable(url_stat_test url_stat_test.cpp)
#target_link_libraries(url_stat_test ${PINPOINT_CPP_LIBRARY})
