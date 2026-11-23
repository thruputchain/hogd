option(X23_COVERAGE "Instrument x23 and its tests for LLVM coverage" OFF)
if(NOT X23_COVERAGE)
  return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "X23_COVERAGE requires Clang and matching LLVM tools")
endif()
if(CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR "X23_COVERAGE requires a native build to run tests")
endif()

string(REGEX MATCH "^[0-9]+" llvm_version "${CMAKE_CXX_COMPILER_VERSION}")
get_filename_component(llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(X23_LLVM_COV NAMES llvm-cov-${llvm_version} llvm-cov
  HINTS "${llvm_bin}" REQUIRED)
find_program(X23_LLVM_PROFDATA NAMES llvm-profdata-${llvm_version} llvm-profdata
  HINTS "${llvm_bin}" REQUIRED)
foreach(tool X23_LLVM_COV X23_LLVM_PROFDATA)
  execute_process(COMMAND "${${tool}}" --version
    OUTPUT_VARIABLE tool_version COMMAND_ERROR_IS_FATAL ANY)
  if(NOT tool_version MATCHES "LLVM version ${llvm_version}\\.")
    message(FATAL_ERROR "${tool} must match Clang ${llvm_version}: ${${tool}}")
  endif()
endforeach()

set(coverage_dir "${CMAKE_CURRENT_BINARY_DIR}/coverage")
option(X23_COVERAGE_CONTINUOUS "Preserve coverage from aborting tests" OFF)
set(coverage_profile_pattern "${coverage_dir}/raw/%m-%p.profraw")
set(coverage_continuous_flags)
if(X23_COVERAGE_CONTINUOUS)
  include(CMakePushCheckState)
  include(CheckCXXCompilerFlag)
  cmake_push_check_state()
  string(APPEND CMAKE_REQUIRED_FLAGS " -fprofile-instr-generate")
  check_cxx_compiler_flag(-fprofile-continuous X23_HAVE_PROFILE_CONTINUOUS)
  cmake_pop_check_state()
  if(NOT X23_HAVE_PROFILE_CONTINUOUS)
    message(FATAL_ERROR "Continuous coverage requires -fprofile-continuous support")
  endif()
  set(coverage_continuous_flags -fprofile-continuous)
  # The runtime needs %c even when LLVM_PROFILE_FILE overrides the
  # compiler's default filename (as the test runner does below).
  set(coverage_profile_pattern "${coverage_dir}/raw/%m-%p%c.profraw")
endif()
get_directory_property(coverage_tests DIRECTORY tests DEFINITION TEST_EXECUTABLES)
set(coverage_objects)
foreach(target x23 x23c ${coverage_tests})
  target_compile_options(${target} PRIVATE -O0 -g -UNDEBUG
    ${coverage_continuous_flags}
    "-fprofile-instr-generate=${coverage_dir}/raw/%m-%p.profraw"
    -fcoverage-mapping -fprofile-update=atomic)
  target_link_options(${target} PRIVATE
    "-fprofile-instr-generate=${coverage_dir}/raw/%m-%p.profraw")
  list(APPEND coverage_objects "$<TARGET_FILE:${target}>")
endforeach()
# Consumers of the instrumented library need the profiling runtime too.
target_link_options(x23 INTERFACE $<BUILD_INTERFACE:-fprofile-instr-generate>)

# Limit reports to maintained sources, excluding tests, dependencies and
# generated files. Including the library archive also counts unlinked code.
file(GLOB coverage_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/x23/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/x23/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/x23c/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/x23c/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/x23c/*.ll"
  "${CMAKE_CURRENT_SOURCE_DIR}/x23c/*.yy")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/coverage-config.cmake.in"
  "${coverage_dir}/config.cmake.in" @ONLY)
file(GENERATE OUTPUT "${coverage_dir}/config-$<CONFIG>.cmake"
  INPUT "${coverage_dir}/config.cmake.in")
add_custom_target(coverage
  COMMAND "${CMAKE_COMMAND}"
    "-DCOVERAGE_CONFIG=${coverage_dir}/config-$<CONFIG>.cmake"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/coverage-run.cmake"
  DEPENDS build_tests x23c
  USES_TERMINAL
  VERBATIM
  COMMENT "Running tests and generating LLVM coverage reports")
