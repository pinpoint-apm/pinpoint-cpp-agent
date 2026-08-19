# Distributed tracing demo: proxy -> server -> (simulated) MySQL (see README.md).
add_executable(distributed_proxy proxy.cpp)
target_link_libraries(distributed_proxy ${PINPOINT_CPP_LIBRARY})

add_executable(distributed_server server.cpp)
target_link_libraries(distributed_server ${PINPOINT_CPP_LIBRARY})
