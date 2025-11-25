# cURL Demo
find_package(CURL QUIET)

if(CURL_FOUND)
    message(STATUS "Found CURL: ${CURL_LIBRARIES}")
    
    add_executable(curl_example curl_web_example.cpp)
    target_include_directories(curl_example PRIVATE . ${CURL_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(curl_example ${PINPOINT_CPP_LIBRARY} ${CURL_LIBRARIES})
else()
    message(STATUS "CURL not found. cURL example will not be built.")
endif()

