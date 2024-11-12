cmake_minimum_required(VERSION 3.18)
project(hogd CXX)

# Configuration parameters
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_COLOR_MAKEFILE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)

# Set up compiler flags for release and debug build types
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -g -march=native")
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_compile_options(-stdlib=libc++ -fexperimental-library)
  add_link_options(-stdlib=libc++ -fexperimental-library)
endif()
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  link_libraries(stdc++exp) # needed for C++23 stacktrace library
endif()
set(CMAKE_CXX_FLAGS_DEBUG "-DDEBUG -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG -O3")

include(cmake/stacktrace.cmake)
include(cmake/wat2wasm.cmake)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/built_headers/hogd/config.h
  @ONLY
)

include_directories(${CMAKE_CURRENT_BINARY_DIR}/built_headers)

# Enable GoogleTest (google's unit testing library)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
option(INSTALL_GTEST "" OFF)
add_subdirectory(external/googletest)
set(GOOGLETEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/googletest"
  CACHE PATH "Path to googletest" FORCE)
enable_testing()
include(GoogleTest)

# Enable performance (google's benchmarking library)
option(BENCHMARK_ENABLE_TESTING "" OFF)
add_subdirectory(external/benchmark)
if(BENCHMARK_ENABLE_TESTING)
set(BENCHMARK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/benchmark"
  CACHE PATH "Path to Google benchmark" FORCE)
endif()

# Detect number of cores - only needed for make-based ExternalProject
include(ProcessorCount)
ProcessorCount(N)

# default to 03 for external project build
set(LIBSODIUM_C_FLAGS "-O3 ${CMAKE_C_FLAGS}")
set(LIBSODIUM_CXX_FLAGS "-O3 ${CMAKE_CXX_FLAGS}")

# Enable libsodium
# --enable-opt sets -march=native and other compiler flags
# that the libsodium authors deem important
include(ExternalProject)
ExternalProject_Add(libsodium
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/external/libsodium
    CONFIGURE_COMMAND
      ${CMAKE_SOURCE_DIR}/external/libsodium/configure
      --prefix=${CMAKE_INSTALL_PREFIX}
      --enable-opt
      CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER}
      CFLAGS=${LIBSODIUM_C_FLAGS} CXXFLAGS=${LIBSODIUM_CXX_FLAGS}
    BUILD_COMMAND make -j${N}
    INSTALL_COMMAND ""  # Skip installation step
)
ExternalProject_Get_Property(libsodium binary_dir)
set(LIBSODIUM_PCFILEDIR "${binary_dir}")
set(LIBSODIUM_TOP_SRCDIR "${CMAKE_SOURCE_DIR}/external/libsodium")
set(LIBSODIUM_INCLUDE_DIRS
    "${LIBSODIUM_PCFILEDIR}/src/libsodium/include"
    "${LIBSODIUM_TOP_SRCDIR}/src/libsodium/include"
    "${LIBSODIUM_TOP_SRCDIR}/src/libsodium/include/sodium"
)
set(LIBSODIUM_LIBRARIES
"${LIBSODIUM_PCFILEDIR}/src/libsodium/.libs/libsodium.a")
include_directories(${LIBSODIUM_INCLUDE_DIRS})

# Enable x23
set(X23_MAKE_ALL_DEPEND_ON_TESTS ON)
add_subdirectory(x23)
include_directories(${X23_SOURCE_DIR} ${X23_BINARY_DIR})

# Enable Corrosion
add_subdirectory(external/corrosion)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/corrosion/cmake")
include(Corrosion)

# Finally, enable hogd's own code
include_directories(${CMAKE_SOURCE_DIR})
add_definitions(-DCMAKE_ROOT_BINARY_DIR=\"${CMAKE_BINARY_DIR}\")
add_subdirectory(hogd)
