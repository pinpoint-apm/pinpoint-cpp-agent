if (BUILD_SHARED_LIBS)
  set(PINPOINT_CPP_LIBRARY pinpoint_cpp)
else()
  set(PINPOINT_CPP_LIBRARY pinpoint_cpp-static)
endif()

#add_executable(pinpoint_tracer_test tracer_test.cpp)
#target_link_libraries(pinpoint_tracer_test ${PINPOINT_CPP_LIBRARY})
#add_test(NAME pinpoint_tracer_test COMMAND pinpoint_tracer_test)
