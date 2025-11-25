# MongoDB Demo
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBMONGOCXX libmongocxx)
    pkg_check_modules(LIBBSONCXX libbsoncxx)
endif()

if(LIBMONGOCXX_FOUND AND LIBBSONCXX_FOUND)
    message(STATUS "Found libmongocxx: ${LIBMONGOCXX_LINK_LIBRARIES}")
    message(STATUS "Found libbsoncxx: ${LIBBSONCXX_LINK_LIBRARIES}")
    
    add_executable(mongo_example mongo_example.cpp)
    target_include_directories(mongo_example PRIVATE 
        . 
        ${LIBMONGOCXX_INCLUDE_DIRS}
        ${LIBBSONCXX_INCLUDE_DIRS}
    )
    target_link_libraries(mongo_example 
        ${PINPOINT_CPP_LIBRARY} 
        ${LIBMONGOCXX_LINK_LIBRARIES}
        ${LIBBSONCXX_LINK_LIBRARIES}
    )
    
    # MongoDB C++ driver requires C++17
    set_target_properties(mongo_example PROPERTIES CXX_STANDARD 17)

else()
    # Fallback: try to find manually if not found via pkg-config
    find_package(mongocxx QUIET)
    find_package(bsoncxx QUIET)

    if(mongocxx_FOUND AND bsoncxx_FOUND)
         message(STATUS "Found mongocxx via CMake config")
         add_executable(mongo_example mongo_example.cpp)
         target_link_libraries(mongo_example 
            ${PINPOINT_CPP_LIBRARY} 
            mongo::mongocxx_shared
            mongo::bsoncxx_shared
         )
         set_target_properties(mongo_example PROPERTIES CXX_STANDARD 17)
    else()
         message(STATUS "libmongocxx not found. MongoDB example will not be built.")
    endif()
endif()

