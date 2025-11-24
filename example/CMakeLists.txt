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

option(BUILD_MYSQL_EXAMPLE "Build MySQL example" OFF)
if (BUILD_MYSQL_EXAMPLE)
    add_subdirectory(mysql_demo)
endif()

option(BUILD_GRPC_EXAMPLE "Build gRPC example" OFF)
if (BUILD_GRPC_EXAMPLE)
    add_subdirectory(grpc_demo)
endif()

option(BUILD_KAFKA_EXAMPLE "Build Kafka example" ON)
if (BUILD_KAFKA_EXAMPLE)
    add_subdirectory(kafka_demo)
endif()
