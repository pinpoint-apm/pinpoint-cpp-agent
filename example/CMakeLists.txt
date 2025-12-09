if (BUILD_SHARED_LIBS)
    set(PINPOINT_CPP_LIBRARY pinpoint_cpp)
else()
    set(PINPOINT_CPP_LIBRARY pinpoint_cpp-static)
endif()

add_executable(tutorial tutorial.cpp)
target_link_libraries(tutorial ${PINPOINT_CPP_LIBRARY})

add_executable(http_server http_server.cpp)
target_link_libraries(http_server ${PINPOINT_CPP_LIBRARY})

# web_demo requires cpptrace - only build if found on system
find_package(cpptrace CONFIG QUIET)

if(cpptrace_FOUND)
  message(STATUS "cpptrace found - building web_demo")
  add_executable(web_demo web_demo.cpp)
  target_link_libraries(web_demo ${PINPOINT_CPP_LIBRARY} cpptrace::cpptrace)
else()
  message(STATUS "cpptrace not found - skipping web_demo build")
  message(STATUS "To build web_demo, install cpptrace or set cpptrace_DIR")
endif()

option(BUILD_MYSQL_EXAMPLE "Build MySQL example" ON)
if (BUILD_MYSQL_EXAMPLE)
    add_subdirectory(mysql_demo)
endif()

option(BUILD_GRPC_EXAMPLE "Build gRPC example" ON)
if (BUILD_GRPC_EXAMPLE)
    add_subdirectory(grpc_demo)
endif()

option(BUILD_KAFKA_EXAMPLE "Build Kafka example" ON)
if (BUILD_KAFKA_EXAMPLE)
    add_subdirectory(kafka_demo)
endif()

option(BUILD_REDIS_EXAMPLE "Build Redis example" ON)
if (BUILD_REDIS_EXAMPLE)
    add_subdirectory(redis_demo)
endif()

option(BUILD_MONGODB_EXAMPLE "Build MongoDB example" ON)
if (BUILD_MONGODB_EXAMPLE)
    add_subdirectory(mongodb_demo)
endif()

option(BUILD_CURL_EXAMPLE "Build cURL example" ON)
if (BUILD_CURL_EXAMPLE)
    add_subdirectory(curl_demo)
endif()

