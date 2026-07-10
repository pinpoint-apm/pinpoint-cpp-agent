cmake_minimum_required(VERSION 3.21)

foreach(_required_variable IN ITEMS
        CTEST_EXECUTABLE
        LLVM_COV_EXECUTABLE
        LLVM_PROFDATA_EXECUTABLE
        COVERAGE_BINARY
        BUILD_DIR
        OUTPUT_DIR)
  if(NOT DEFINED ${_required_variable} OR "${${_required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${_required_variable} is required")
  endif()
endforeach()

set(_raw_profile_dir "${OUTPUT_DIR}/raw")
set(_profile_data "${OUTPUT_DIR}/coverage.profdata")
set(_summary_file "${OUTPUT_DIR}/coverage.txt")
set(_html_dir "${OUTPUT_DIR}/html")

# Start each run from an empty directory so a report never includes stale test
# data. %p prevents forked/multi-process tests from overwriting one another and
# %m distinguishes instrumented binaries and shared libraries.
file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${_raw_profile_dir}" "${_html_dir}")
set(_raw_profile_pattern "${_raw_profile_dir}/%p-%m.profraw")

set(_ctest_command
    "${CTEST_EXECUTABLE}"
    --test-dir "${BUILD_DIR}"
    --output-on-failure)
if(DEFINED COVERAGE_CONFIG AND NOT "${COVERAGE_CONFIG}" STREQUAL "")
  list(APPEND _ctest_command --build-config "${COVERAGE_CONFIG}")
endif()

message(STATUS "Running tests with LLVM coverage instrumentation")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "LLVM_PROFILE_FILE=${_raw_profile_pattern}"
          ${_ctest_command}
  RESULT_VARIABLE _ctest_result)
if(NOT "${_ctest_result}" STREQUAL "0")
  message(FATAL_ERROR
    "CTest failed with exit code ${_ctest_result}; coverage was not generated")
endif()

file(GLOB _raw_profiles LIST_DIRECTORIES false
     "${_raw_profile_dir}/*.profraw")
list(SORT _raw_profiles)
list(LENGTH _raw_profiles _raw_profile_count)
if(_raw_profile_count EQUAL 0)
  message(FATAL_ERROR
    "Tests passed but no .profraw files were written to ${_raw_profile_dir}")
endif()

message(STATUS "Merging ${_raw_profile_count} raw profile file(s)")
execute_process(
  COMMAND "${LLVM_PROFDATA_EXECUTABLE}" merge -sparse
          ${_raw_profiles} "-o=${_profile_data}"
  RESULT_VARIABLE _merge_result
  ERROR_VARIABLE _merge_error)
if(NOT "${_merge_result}" STREQUAL "0")
  message(FATAL_ERROR
    "llvm-profdata merge failed (${_merge_result}):\n${_merge_error}")
endif()

set(_llvm_cov_arguments
    "${COVERAGE_BINARY}"
    "-instr-profile=${_profile_data}")
if(DEFINED COVERAGE_IGNORE_REGEX AND
   NOT "${COVERAGE_IGNORE_REGEX}" STREQUAL "")
  list(APPEND _llvm_cov_arguments
       "-ignore-filename-regex=${COVERAGE_IGNORE_REGEX}")
endif()

execute_process(
  COMMAND "${LLVM_COV_EXECUTABLE}" report ${_llvm_cov_arguments}
  RESULT_VARIABLE _report_result
  OUTPUT_VARIABLE _coverage_summary
  ERROR_VARIABLE _report_error)
if(NOT "${_report_result}" STREQUAL "0")
  message(FATAL_ERROR
    "llvm-cov report failed (${_report_result}):\n${_report_error}")
endif()

file(WRITE "${_summary_file}" "${_coverage_summary}")
message("${_coverage_summary}")

execute_process(
  COMMAND "${LLVM_COV_EXECUTABLE}" show ${_llvm_cov_arguments}
          -format=html "-output-dir=${_html_dir}"
  RESULT_VARIABLE _html_result
  OUTPUT_QUIET
  ERROR_VARIABLE _html_error)
if(NOT "${_html_result}" STREQUAL "0")
  message(FATAL_ERROR
    "llvm-cov HTML generation failed (${_html_result}):\n${_html_error}")
endif()

if(NOT EXISTS "${_html_dir}/index.html")
  message(FATAL_ERROR
    "llvm-cov completed without creating ${_html_dir}/index.html")
endif()

message(STATUS "Coverage summary: ${_summary_file}")
message(STATUS "Coverage HTML: ${_html_dir}/index.html")
