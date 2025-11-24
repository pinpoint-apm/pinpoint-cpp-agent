# MySQL Connector C++ X DevAPI support
find_path(MYSQLCPPCONN_INCLUDE_DIR mysqlx/xdevapi.h
    PATHS /usr/local/include /opt/homebrew/include /usr/include
    PATH_SUFFIXES mysql-connector-c++
)

find_library(MYSQLCPPCONNX_LIBRARY mysqlcppconnx
    PATHS /usr/local/lib /opt/homebrew/lib /usr/lib
    PATH_SUFFIXES mysql-connector-c++
)

if(MYSQLCPPCONN_INCLUDE_DIR AND MYSQLCPPCONNX_LIBRARY)
    message(STATUS "Found MySQL Connector C++ X DevAPI")
    message(STATUS "  Include dir: ${MYSQLCPPCONN_INCLUDE_DIR}")
    message(STATUS "  Library: ${MYSQLCPPCONNX_LIBRARY}")
    
    add_executable(db_demo db_demo.cpp)
    target_include_directories(db_demo PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${MYSQLCPPCONN_INCLUDE_DIR}
    )
    target_link_libraries(db_demo ${PINPOINT_CPP_LIBRARY} ${MYSQLCPPCONNX_LIBRARY})
    
    # Set C++ standard to 14 or higher (required by MySQL Connector C++)
    set_target_properties(db_demo PROPERTIES CXX_STANDARD 14)
else()
    message(WARNING "MySQL Connector C++ X DevAPI not found. db_demo will not be built.")
    message(STATUS "  To install on macOS: brew install mysql-connector-c++")
    message(STATUS "  To install on Ubuntu: apt-get install libmysqlcppconn-dev")
endif()

