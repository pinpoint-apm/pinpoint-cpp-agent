# Redis Demo
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(Hiredis hiredis)
endif()

if(Hiredis_FOUND)
    message(STATUS "Found hiredis: ${Hiredis_LINK_LIBRARIES}")
    
    add_executable(redis_example redis_example.cpp)
    target_include_directories(redis_example PRIVATE . ${Hiredis_INCLUDE_DIRS})
    target_link_libraries(redis_example ${PINPOINT_CPP_LIBRARY} ${Hiredis_LINK_LIBRARIES})
else()
    # Manual fallback or check if FetchContent was used in root
    find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h
        PATHS /usr/local/include /opt/homebrew/include /usr/include
    )
    find_library(HIREDIS_LIBRARY hiredis
        PATHS /usr/local/lib /opt/homebrew/lib /usr/lib
    )
    
    if(HIREDIS_INCLUDE_DIR AND HIREDIS_LIBRARY)
        message(STATUS "Found hiredis manually")
        add_executable(redis_example redis_example.cpp)
        target_include_directories(redis_example PRIVATE . ${HIREDIS_INCLUDE_DIR})
        target_link_libraries(redis_example ${PINPOINT_CPP_LIBRARY} ${HIREDIS_LIBRARY})
    else()
        FetchContent_Declare(
        hiredis
        GIT_REPOSITORY https://github.com/redis/hiredis.git
        GIT_TAG        v1.2.0
        )
        set(ENABLE_SSL OFF CACHE BOOL "" FORCE)
        set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(hiredis)

        if(TARGET hiredis)
           message(STATUS "Using hiredis from FetchContent")
           add_executable(redis_example redis_example.cpp)
           target_link_libraries(redis_example ${PINPOINT_CPP_LIBRARY} hiredis::hiredis)
        else()
           message(STATUS "hiredis not found. Redis example will not be built.")
        endif()
    endif()
endif()
