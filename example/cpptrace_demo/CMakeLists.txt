# cpptrace Demo
find_package(cpptrace CONFIG QUIET)

if(cpptrace_FOUND)
    message(STATUS "Found cpptrace - building cpptrace_demo")
    
    add_executable(cpptrace_demo cpptrace_demo.cpp)
    target_include_directories(cpptrace_demo PRIVATE . ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(cpptrace_demo ${PINPOINT_CPP_LIBRARY} cpptrace::cpptrace)
else()
    message(STATUS "cpptrace not found - skipping cpptrace_demo build")
    message(STATUS "To build cpptrace_demo, install cpptrace or set cpptrace_DIR")
endif()

