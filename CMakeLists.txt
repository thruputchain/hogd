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

# check for C++23 stacktrace library
include(CheckIncludeFileCXX)
CHECK_INCLUDE_FILE_CXX(stacktrace STACKTRACE_INCLUDE)
IF(STACKTRACE_INCLUDE)
   add_definitions(-DHAVE_CXX_STACKTRACE)
ENDIF(STACKTRACE_INCLUDE)

include(FetchContent)

# Enable GoogleTest (google's unit testing library)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
option(INSTALL_GTEST "" OFF)
add_subdirectory(external/googletest)
enable_testing()
include(GoogleTest)

# Enable performance (google's benchmarking library)
option(BENCHMARK_ENABLE_TESTING "" OFF)
add_subdirectory(external/benchmark)

# Detect number of cores - only needed for make-based ExternalProject
include(ProcessorCount)
ProcessorCount(N)

# Enable libsodium
include(ExternalProject)
ExternalProject_Add(libsodium
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/external/libsodium
    CONFIGURE_COMMAND
      ${CMAKE_SOURCE_DIR}/external/libsodium/configure
      --prefix=${CMAKE_INSTALL_PREFIX}
      CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER}
      CFLAGS=${CMAKE_C_FLAGS} CXXFLAGS=${CMAKE_CXX_FLAGS}
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

add_subdirectory(external/corrosion)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/corrosion/cmake")
include(Corrosion)

# Add code directories
include_directories(${CMAKE_SOURCE_DIR})

add_subdirectory(hogd)

