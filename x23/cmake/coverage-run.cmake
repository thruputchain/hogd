cmake_minimum_required(VERSION 3.28)
include("${COVERAGE_CONFIG}")

# Never mix earlier runs (or compiler invocations during the build) into
# this run. Remove stale reports too, in case collecting profiles fails.
file(REMOVE_RECURSE "${coverage_dir}/raw" "${coverage_dir}/html")
file(REMOVE "${coverage_dir}/summary.txt" "${coverage_dir}/coverage.profdata")
file(MAKE_DIRECTORY "${coverage_dir}/raw")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "LLVM_PROFILE_FILE=${coverage_dir}/raw/%m-%p.profraw"
    "${coverage_ctest}" --test-dir "${coverage_build_dir}"
    -C "${coverage_config}" --output-on-failure --parallel
  RESULT_VARIABLE test_result)

file(GLOB profiles "${coverage_dir}/raw/*.profraw")
if(NOT profiles)
  message(FATAL_ERROR "No coverage profiles produced (CTest result: ${test_result})")
endif()
execute_process(
  COMMAND "${coverage_profdata}" merge -sparse ${profiles}
    -o "${coverage_dir}/coverage.profdata"
  COMMAND_ERROR_IS_FATAL ANY)
list(POP_FRONT coverage_objects main_object)
set(objects)
foreach(object IN LISTS coverage_objects)
  list(APPEND objects -object "${object}")
endforeach()
set(report_args "${main_object}" ${objects} "-instr-profile=${coverage_dir}/coverage.profdata")
execute_process(
  COMMAND "${coverage_cov}" report ${report_args} ${coverage_sources}
  OUTPUT_FILE "${coverage_dir}/summary.txt"
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(
  COMMAND "${coverage_cov}" show ${report_args} ${coverage_sources}
    -format=html "-output-dir=${coverage_dir}/html"
    -show-line-counts-or-regions -show-branches=count
  COMMAND_ERROR_IS_FATAL ANY)
file(READ "${coverage_dir}/summary.txt" summary)
message("${summary}")
message(STATUS "Coverage report: ${coverage_dir}/html/index.html")
# Keep partial reports useful for diagnosis, but never hide test failures.
if(NOT test_result STREQUAL "0")
  message(FATAL_ERROR "CTest failed (${test_result}); coverage report is partial")
endif()
