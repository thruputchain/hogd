cmake_minimum_required(VERSION 3.18)
project(hogd C CXX)

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

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/built_headers/hogd/config.h
  @ONLY
)

include(CheckIPOSupported)
check_ipo_supported()

include_directories(${CMAKE_CURRENT_BINARY_DIR}/built_headers)

# Enable GoogleTest (Google's unit testing library)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
option(INSTALL_GTEST "" OFF)
add_subdirectory(external/googletest)
set(GOOGLETEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/googletest"
  CACHE PATH "Path to googletest" FORCE)
enable_testing()
include(GoogleTest)

add_library(gtest_headers INTERFACE)
target_include_directories(gtest_headers INTERFACE
    ${gtest_SOURCE_DIR}/include
)

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

if (CMAKE_BUILD_TYPE STREQUAL "Release")
  set(LIBSODIUM_C_FLAGS "${EXTERNAL_C_FLAGS} -Wno-unused-function")
  set(LIBSODIUM_CXX_FLAGS "${EXTERNAL_CXX_FLAGS} -Wno-unused-function")
else()
  # Turn on minimal optimization for libsodium, so that FORTIFY_SOURCE is active for both debug and release builds
  # On the presumption that during debugging, we do want the FORTIFY_SOURCE checks to be active
  # on the libsodium code.
  set(LIBSODIUM_C_FLAGS "${EXTERNAL_C_FLAGS} -Wno-unused-function -O1")
  set(LIBSODIUM_CXX_FLAGS "${EXTERNAL_CXX_FLAGS} -Wno-unused-function -O1")
endif()

if (HAS_CI_FAIL_LOG)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CMAKE_SOURCE_DIR}/cmake/fail_wrapper.sh ${ENABLE_CI_FAIL_LOG}")
  set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK    "${CMAKE_SOURCE_DIR}/cmake/fail_wrapper.sh ${ENABLE_CI_FAIL_LOG}")
endif()

# Enable libsodium
# --enable-opt sets -march=native and other compiler flags
# that the libsodium authors deem important
include(ExternalProject)
ExternalProject_Add(libsodium_build
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/external/libsodium
    BINARY_DIR ${CMAKE_BINARY_DIR}/libsodium_build
    CONFIGURE_COMMAND
      ${CMAKE_SOURCE_DIR}/external/libsodium/configure 
      --prefix=<BINARY_DIR>
      --enable-opt
      --disable-pie
      CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER}
      CFLAGS=${LIBSODIUM_C_FLAGS}
      CXXFLAGS=${LIBSODIUM_CXX_FLAGS}
    BUILD_COMMAND make -j${N}
    INSTALL_COMMAND "" # Skip installation step
)
ExternalProject_Add_StepTargets(libsodium_build configure)

ExternalProject_Get_Property(libsodium_build binary_dir)
set(LIBSODIUM_PCFILEDIR "${binary_dir}")
set(LIBSODIUM_TOP_SRCDIR "${CMAKE_SOURCE_DIR}/external/libsodium")
set(LIBSODIUM_INCLUDE_DIRS
    "${LIBSODIUM_PCFILEDIR}/src/libsodium/include"
    "${LIBSODIUM_TOP_SRCDIR}/src/libsodium/include"
    "${LIBSODIUM_TOP_SRCDIR}/src/libsodium/include/sodium"
)
include_directories(${LIBSODIUM_INCLUDE_DIRS})

set(LIBSODIUM_LIBRARY
"${LIBSODIUM_PCFILEDIR}/src/libsodium/.libs/libsodium${CMAKE_STATIC_LIBRARY_SUFFIX}")

add_library(libsodium STATIC IMPORTED)
set_target_properties(libsodium PROPERTIES IMPORTED_LOCATION "${LIBSODIUM_LIBRARY}")
add_dependencies(libsodium libsodium-build)
# Enable x23
set(X23_MAKE_ALL_DEPEND_ON_TESTS ON)
add_subdirectory(x23)
include_directories(${X23_SOURCE_DIR} ${X23_BINARY_DIR})

# Enable Corrosion
add_subdirectory(external/corrosion)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/corrosion/cmake")
include(Corrosion)

# Enable bcs-cpp
add_subdirectory(external/bcs-cpp)
set(BCS_CPP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/bcs-cpp")
include_directories(${BCS_CPP_SOURCE_DIR})

# Enable model-checker
add_subdirectory(external/model-checker)
set(MODEL_CHECKER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/model-checker")
include_directories(${MODEL_CHECKER_SOURCE_DIR})

add_subdirectory(built_contracts)

# Finally, enable hogd's own code
include_directories(${CMAKE_SOURCE_DIR})
add_definitions(-DCMAKE_ROOT_BINARY_DIR=\"${CMAKE_BINARY_DIR}\")
add_subdirectory(hogd)