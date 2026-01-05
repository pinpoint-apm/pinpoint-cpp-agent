cmake_minimum_required(VERSION 3.10)

# MySQL Connector C++ X DevAPI support
find_path(MYSQLCPPCONN_INCLUDE_DIR mysqlx/xdevapi.h
    PATHS /usr/local/include /opt/homebrew/include /usr/include
    PATH_SUFFIXES mysql-connector-c++
)

find_library(MYSQLCPPCONNX_LIBRARY mysqlcppconnx
    PATHS /usr/local/lib /opt/homebrew/lib /usr/lib
    PATH_SUFFIXES mysql-connector-c++
)

# Find nlohmann_json
find_package(nlohmann_json 3.2.0 QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found via CMake, trying pkg-config")
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(NLOHMANN_JSON nlohmann_json)
    endif()
    
    if(NOT NLOHMANN_JSON_FOUND)
        message(WARNING "nlohmann_json not found. Please install:\n"
                       "  Ubuntu/Debian: sudo apt-get install nlohmann-json3-dev\n"
                       "  macOS: brew install nlohmann-json")
    endif()
endif()

if(MYSQLCPPCONN_INCLUDE_DIR AND MYSQLCPPCONNX_LIBRARY)
    message(STATUS "Found MySQL Connector C++ X DevAPI")
    message(STATUS "  Include dir: ${MYSQLCPPCONN_INCLUDE_DIR}")
    message(STATUS "  Library: ${MYSQLCPPCONNX_LIBRARY}")
    
    # Create executable
    add_executable(todolist_server todolist_server.cpp)
    
    # Include directories
    target_include_directories(todolist_server PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/3rd_party
        ${CMAKE_SOURCE_DIR}/example
        ${MYSQLCPPCONN_INCLUDE_DIR}
    )
    
    # Link libraries
    target_link_libraries(todolist_server PRIVATE
        ${PINPOINT_CPP_LIBRARY} 
        ${MYSQLCPPCONNX_LIBRARY}
    )
    
    # Link nlohmann/json
    if(nlohmann_json_FOUND)
        target_link_libraries(todolist_server PRIVATE nlohmann_json::nlohmann_json)
    elseif(NLOHMANN_JSON_FOUND)
        target_include_directories(todolist_server PRIVATE ${NLOHMANN_JSON_INCLUDE_DIRS})
    endif()
    
    # Platform-specific settings
    if(UNIX AND NOT APPLE)
        target_link_libraries(todolist_server PRIVATE pthread dl)
    elseif(APPLE)
        target_link_libraries(todolist_server PRIVATE pthread)
    endif()
    
    # Set C++ standard to 14 or higher (required by MySQL Connector C++)
    set_target_properties(todolist_server PROPERTIES CXX_STANDARD 14)
    
    # Copy config file to build directory
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/pinpoint-config.yaml)
        configure_file(
            ${CMAKE_CURRENT_SOURCE_DIR}/pinpoint-config.yaml
            ${CMAKE_CURRENT_BINARY_DIR}/pinpoint-config.yaml
            COPYONLY
        )
    endif()
else()
    message(WARNING "MySQL Connector C++ X DevAPI not found. todolist_server will not be built.")
    message(STATUS "  To install on macOS: brew install mysql-connector-c++")
    message(STATUS "  To install on Ubuntu: apt-get install libmysqlcppconn-dev")
endif()

