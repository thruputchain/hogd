cmake_minimum_required(VERSION 3.18)
project(hogd VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_POLICY_VERSION_MINIMUM 3.18)

# Configuration parameters
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_COLOR_MAKEFILE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)

# Configuration options
# Enable building test ffi (default: OFF)
# Turning test ffi on requires building with fPIC
option(ENABLE_TEST_FFI "Enable mock contracts & test FFI" OFF)
# Enable logging of build failures to a file
# This is used to track build failures in CI,
# so that we can see what failed more easily (without info buried under pages of logs).
set(ENABLE_CI_FAIL_LOG "" CACHE STRING "Path to log file for build failures")
if(ENABLE_CI_FAIL_LOG)
  set(HAS_CI_FAIL_LOG ON)
else()
  set(HAS_CI_FAIL_LOG OFF)
endif()

message(STATUS "ENABLE_CI_FAIL_LOG: ${ENABLE_CI_FAIL_LOG}")
message(STATUS "HAS_CI_FAIL_LOG: ${HAS_CI_FAIL_LOG}")

# Get git commit hash
find_package(Git)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE git_status
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(git_status)
        set(GIT_COMMIT_HASH "unknown")
    endif()
else()
    set(GIT_COMMIT_HASH "unknown")
endif()

# Print configuration options
message(STATUS "ENABLE_TEST_FFI: ${ENABLE_TEST_FFI}")
message(STATUS "GIT_COMMIT_HASH: ${GIT_COMMIT_HASH}")

# Handle configuration options
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
if(ENABLE_TEST_FFI)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Set up compiler flags for release and debug build types
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -g -march=native")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -g -march=native")
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_compile_options(-stdlib=libc++ -fexperimental-library)
  add_link_options(-stdlib=libc++ -fexperimental-library)
endif()
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-interference-size")
  link_libraries(stdc++exp) # needed for C++23 stacktrace library
endif()
set(CMAKE_C_FLAGS_DEBUG "-DDEBUG -O0")
set(CMAKE_C_FLAGS_RELEASE "-DNDEBUG -O3")
set(CMAKE_CXX_FLAGS_DEBUG "-DDEBUG -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG -O3")

include(cmake/stacktrace.cmake)
include(cmake/wat2wasm.cmake)
include(cmake/wasmopt.cmake)

# Where to place generated headers
set(BUILT_HEADERS_DIR "${CMAKE_CURRENT_BINARY_DIR}/built_headers")
file(MAKE_DIRECTORY "${BUILT_HEADERS_DIR}")

set(HOGD_BUILT_HEADERS_DIR "${BUILT_HEADERS_DIR}/hogd")
file(MAKE_DIRECTORY "${HOGD_BUILT_HEADERS_DIR}")

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.in
  ${HOGD_BUILT_HEADERS_DIR}/config.h
  @ONLY
)

# A target that regenerates version.cc on every build.
set(VERSION_CC "${HOGD_BUILT_HEADERS_DIR}/version.cc")
add_custom_target(version ALL
  COMMAND ${CMAKE_COMMAND}
          -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -DOUT_FILE=${VERSION_CC}
          -DPROJECT_VERSION=${PROJECT_VERSION}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake
  BYPRODUCTS "${VERSION_CC}"
  COMMENT "Generating version.cc"
  VERBATIM)

# === Interface target that exposes the header and enforces the dep ===
add_library(built_headers_interface INTERFACE)
# Only this target provides the include dir where the header lives:
target_include_directories(built_headers_interface INTERFACE "${BUILT_HEADERS_DIR}")
# Make anyone who links this depend on header generation:
add_dependencies(built_headers_interface version)

include(CheckIPOSupported)
check_ipo_supported()

# Enable GoogleTest (Google's unit testing library)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
option(INSTALL_GTEST "" OFF)
add_subdirectory(external/googletest)
set(GOOGLETEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/googletest"
  CACHE PATH "Path to googletest" FORCE)
enable_testing()
include(GoogleTest)

# Note: One can extract the include directory from the gtest library
# with the following snippet:
#add_library(gtest_headers INTERFACE)
#target_include_directories(gtest_headers INTERFACE
#    ${gtest_SOURCE_DIR}/include
#)
# This may slightly improves build parallelism (the base/crypto tests don't need to wait
# to start building for the gtest library to build) depending on gtest_headers causes all sorts
# of false-positive clang-tidy errors.


# Enable performance (Google's benchmarking library)
option(BENCHMARK_ENABLE_TESTING "" OFF)
add_subdirectory(external/benchmark)
if(BENCHMARK_ENABLE_TESTING)
set(BENCHMARK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/benchmark"
  CACHE PATH "Path to Google benchmark" FORCE)
endif()

# Enable crc32c (Google's CRC library)
option(CRC32C_BUILD_TESTS "" OFF)
option(CRC32C_BUILD_BENCHMARKS "" OFF)
option(CRC32C_USE_GLOG "" OFF)
option(CRC32C_INSTALL "" OFF)
add_subdirectory(external/crc32c)

# Enable blake3
add_subdirectory(external/blake3/c)

# Detect number of cores - only needed for make-based ExternalProject
include(ProcessorCount)
ProcessorCount(N)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
set(EXTERNAL_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_RELEASE}")
set(EXTERNAL_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}")
else()
set(EXTERNAL_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
set(EXTERNAL_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_DEBUG}")
endif()

# Related to how dependencies are tracked in ExternalProject_Add_StepTargets
cmake_policy(SET CMP0114 NEW)

if (HAS_CI_FAIL_LOG)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CMAKE_SOURCE_DIR}/cmake/fail_wrapper.sh ${ENABLE_CI_FAIL_LOG}")
  set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK    "${CMAKE_SOURCE_DIR}/cmake/fail_wrapper.sh ${ENABLE_CI_FAIL_LOG}")
endif()


# Enable x23
set(X23_MAKE_ALL_DEPEND_ON_TESTS ON)
add_subdirectory(x23)
include_directories(${X23_SOURCE_DIR} ${X23_BINARY_DIR})

# Enable Corrosion
add_subdirectory(external/corrosion)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/corrosion/cmake")
include(Corrosion)

# Enable model-checker
add_subdirectory(external/model-checker)
set(MODEL_CHECKER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/model-checker")
include_directories(${MODEL_CHECKER_SOURCE_DIR})

add_subdirectory(built_contracts)

# Finally, enable hogd's own code
include_directories(${CMAKE_SOURCE_DIR})
add_definitions(-DCMAKE_ROOT_BINARY_DIR=\"${CMAKE_BINARY_DIR}\")
add_subdirectory(hogd)
