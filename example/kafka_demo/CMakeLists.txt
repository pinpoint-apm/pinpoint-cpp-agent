# Kafka Demo
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(RdKafka rdkafka++)
endif()

if(RdKafka_FOUND)
    message(STATUS "Found rdkafka++: ${RdKafka_LINK_LIBRARIES}")
    
    add_executable(kafka_producer kafka_web_producer.cpp)
    target_include_directories(kafka_producer PRIVATE 
        . 
        ${RdKafka_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/.. 
    )
    target_link_libraries(kafka_producer ${PINPOINT_CPP_LIBRARY} ${RdKafka_LINK_LIBRARIES})

    add_executable(kafka_consumer consumer.cpp)
    target_include_directories(kafka_consumer PRIVATE . ${RdKafka_INCLUDE_DIRS})
    target_link_libraries(kafka_consumer ${PINPOINT_CPP_LIBRARY} ${RdKafka_LINK_LIBRARIES})
else()
    message(STATUS "librdkafka++ not found. Kafka examples will not be built.")
endif()
