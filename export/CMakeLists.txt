# =============================================================================
# C Wrapper Library for Pinpoint C++ Agent
# =============================================================================
# This library provides C-compatible wrapper functions for using the
# Pinpoint C++ agent from C applications.
# =============================================================================

set(C_WRAPPER_SRCS pinpoint_c.cpp)

if(BUILD_SHARED_LIBS)
  add_library(pinpoint_c SHARED ${C_WRAPPER_SRCS})
  target_compile_definitions(pinpoint_c PRIVATE PINPOINT_C_EXPORTS)
  target_include_directories(pinpoint_c 
    PUBLIC 
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
      $<INSTALL_INTERFACE:include>
    PRIVATE
      ${CMAKE_SOURCE_DIR}/include
  )
  target_link_libraries(pinpoint_c PRIVATE pinpoint_cpp)
  set_target_properties(pinpoint_c PROPERTIES 
    VERSION ${PROJECT_VERSION} 
    SOVERSION ${PROJECT_VERSION_MAJOR}
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
  )
  install(TARGETS pinpoint_c LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if(BUILD_STATIC_LIBS)
  add_library(pinpoint_c-static STATIC ${C_WRAPPER_SRCS})
  target_compile_definitions(pinpoint_c-static PRIVATE PINPOINT_C_EXPORTS)
  set_target_properties(pinpoint_c-static PROPERTIES OUTPUT_NAME pinpoint_c)
  target_include_directories(pinpoint_c-static 
    PUBLIC 
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
      $<INSTALL_INTERFACE:include>
    PRIVATE
      ${CMAKE_SOURCE_DIR}/include
  )
  target_link_libraries(pinpoint_c-static PRIVATE pinpoint_cpp-static)
  install(TARGETS pinpoint_c-static ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

# Install C wrapper header
install(FILES pinpoint_c.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Add CivetWeb example (optional, only if user wants to build it)
option(BUILD_CIVETWEB_EXAMPLE "Build CivetWeb integration example" ON)
if(BUILD_CIVETWEB_EXAMPLE)
  add_subdirectory(civetweb_example)
endif()
