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
set(CMAKE_CXX_FLAGS_DEBUG "-DDEBUG -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG -O3")

# Fetch and enable GoogleTest
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
enable_testing()
include(GoogleTest)

# Add code directories
include_directories(${CMAKE_SOURCE_DIR})
add_subdirectory(hogd)

include(ExternalProject)

# Define an external project for the 3rd-party product
ExternalProject_Add(libsodium
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/external/libsodium
    CONFIGURE_COMMAND
      ${CMAKE_SOURCE_DIR}/external/libsodium/configure
      --prefix=${CMAKE_INSTALL_PREFIX}
      CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER}
      CFLAGS=${CMAKE_C_FLAGS} CXXFLAGS=${CMAKE_CXX_FLAGS}
    BUILD_COMMAND make
    INSTALL_COMMAND ""  # Skip installation step
)

ExternalProject_Get_Property(libsodium binary_dir)
set(LIBSODIUM_UNINSTALLED_PKGCONFIG_DIR "${binary_dir}")
set(ENV{PKG_CONFIG_PATH} "${LIBSODIUM_UNINSTALLED_PKGCONFIG_DIR}:$ENV{PKG_CONFIG_PATH}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSODIUM REQUIRED libsodium-uninstalled)
