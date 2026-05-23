cmake_minimum_required(VERSION 3.28)
project(x23
  VERSION 0.0.0
  DESCRIPTION "C++23 XDR/RPC compiler and library"
  HOMEPAGE_URL "https://github.com/groundhogchain/x23/"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(USE_LIBCXX OFF)
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(STATUS "Using Clang, setting libc++")
  add_compile_options(-stdlib=libc++ -fexperimental-library)
  add_link_options(-stdlib=libc++ -fexperimental-library)
  set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -stdlib=libc++ -fexperimental-library")
  set(USE_LIBCXX ON)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(STATUS "Using GCC, adding libstdc++exp")
  list(APPEND CMAKE_REQUIRED_LIBRARIES stdc++exp)
  link_libraries(stdc++exp) # needed for C++23 stacktrace library
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(X23_IS_TOP_LEVEL TRUE)
endif()

if(X23_IS_TOP_LEVEL)
  option(BUILD_BENCHMARKS "Build benchmarks" ON)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  if(NOT CMAKE_CXX_FLAGS)
    set(CMAKE_CXX_FLAGS "-g -O2 -Wall" CACHE STRING "Default C++ flags" FORCE)
  endif()
endif()

# Option for user to override the CPU set size at configure time
set(X23_CPUSET_NCPUS 0 CACHE STRING
    "Number of CPUs in fixed CpuSet (0 means use system CPU_SETSIZE)")

set(X23_FD_TRACE "" CACHE STRING
  "Enable coroutine tracing (0=off, 1=on, empty=auto based on DEBUG)")

set(X23_CORO_TRACE "" CACHE STRING
  "Enable coroutine tracing (0=off, 1=on, empty=auto based on NDEBUG)")

include(cmake/accept4.cmake)
include(cmake/atomicsharedptr.cmake)
include(cmake/demangle.cmake)
include(cmake/epoll.cmake)
include(cmake/getcpu.cmake)
include(cmake/getprogname.cmake)
include(cmake/pause.cmake)
include(cmake/stacktrace.cmake)
include(cmake/coroICE.cmake)

# Resolve X23_CORO_TRACE to a concrete 0 or 1 here in CMake (rather than
# leaving the auto-detect to the preprocessor in corotrace.h) so that we
# can use #cmakedefine01 in config.h.in.  Plain `#cmakedefine X @X@` is a
# footgun: CMake evaluates the value as a boolean (0/OFF/FALSE/NO/"" are
# all falsy), so setting the cache var to "0" silently emits `#undef X`
# instead of `#define X 0`.  With auto-detect living in the header, that
# `#undef` would flip tracing back on in Debug builds, defeating the
# explicit-off case.  Compute the value here, use #cmakedefine01, and the
# header always sees a concrete 0/1.
if(X23_CORO_TRACE STREQUAL "")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND X23_CORO_SLOC_OK)
        set(X23_CORO_TRACE 1)
    else()
        set(X23_CORO_TRACE 0)
    endif()
endif()
message(STATUS "X23_CORO_TRACE = ${X23_CORO_TRACE}")

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/x23/config.h
  @ONLY
)

set(X23_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(X23_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

# Determine the x23c executable
if(CMAKE_CROSSCOMPILING)
  find_program(X23C_EXECUTABLE x23c REQUIRED)
else()
  set(X23C_EXECUTABLE "$<TARGET_FILE:x23c>")
endif()
message(STATUS "X23C_EXECUTABLE is ${X23C_EXECUTABLE}")

if(NOT X23_IS_TOP_LEVEL)
  set(X23_SOURCE_DIR ${X23_SOURCE_DIR} PARENT_SCOPE)
  set(X23_BINARY_DIR ${X23_BINARY_DIR} PARENT_SCOPE)
  set(X23C_EXECUTABLE ${X23C_EXECUTABLE} PARENT_SCOPE)
endif()

function(add_xdr target_name)
  set(xdr_headers)
  foreach(xdr_file IN LISTS ARGN)
    set(xdr_file_ "${CMAKE_CURRENT_SOURCE_DIR}/${xdr_file}")
    if (EXISTS ${xdr_file_})
    else()
      set(xdr_file_ "${CMAKE_CURRENT_BINARY_DIR}/${xdr_file}")
    endif()
    get_filename_component(base_name ${xdr_file_} NAME_WLE)
    set(output_file "${CMAKE_CURRENT_BINARY_DIR}/${base_name}.hh")
    add_custom_command(
      OUTPUT ${output_file}
      COMMAND ${X23C_EXECUTABLE} -o ${output_file} ${xdr_file_}
      DEPENDS ${xdr_file_} ${X23C_EXECUTABLE}
      COMMENT "Generating ${output_file} from ${xdr_file_}"
    )
    list(APPEND xdr_headers ${output_file})
  endforeach()

  add_library(${target_name} INTERFACE)
  target_include_directories(${target_name}
    INTERFACE "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(${target_name} INTERFACE x23)

  add_custom_target(build_${target_name} ALL DEPENDS ${xdr_headers})
  add_dependencies(${target_name} build_${target_name})
endfunction()

function(add_xdr_rs target_name output_folder)
  set(xdr_headers)
  foreach(xdr_file IN LISTS ARGN)
    set(xdr_file_ "${CMAKE_CURRENT_SOURCE_DIR}/${xdr_file}")
    if (EXISTS ${xdr_file_})
    else()
      set(xdr_file_ "${CMAKE_CURRENT_BINARY_DIR}/${xdr_file}")
    endif()
    get_filename_component(base_name ${xdr_file_} NAME_WLE)
    set(output_file "${output_folder}/${base_name}.rs")
    add_custom_command(
      OUTPUT ${output_file}
      COMMAND ${X23C_EXECUTABLE} --rust --rust-no-feature serde --rust-no-feature schema --rust-feature xdrs --rust-no-feature spec -o ${output_file} ${xdr_file_}
      DEPENDS ${xdr_file_} ${X23C_EXECUTABLE}
      COMMENT "Generating ${output_file} from ${xdr_file_}"
    )
    list(APPEND xdr_headers ${output_file})
  endforeach()

  add_library(${target_name} INTERFACE)
  
  add_custom_target(build_${target_name} ALL DEPENDS ${xdr_headers})
  add_dependencies(${target_name} build_${target_name})
endfunction()

function(use_x23 target)
  get_target_property(target_sources ${target} SOURCES)
  set(generated_headers)

  foreach(source ${target_sources})
    if(${source} MATCHES ".*\\.x$")
      get_filename_component(basename ${source} NAME_WLE)
      set(output_file "${CMAKE_CURRENT_BINARY_DIR}/${basename}.hh")
      add_custom_command(
        OUTPUT ${output_file}
        COMMAND ${X23C_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${source}
        DEPENDS ${X23C_EXECUTABLE} ${source}
        COMMENT "Generating ${output_file} from ${source}"
      )
      list(APPEND generated_headers ${output_file})
    endif()
  endforeach()

  if(generated_headers)
    add_custom_target(${target}_generate_x23_headers
      DEPENDS ${generated_headers}
    )
    add_dependencies(${target} ${target}_generate_x23_headers)
    target_sources(${target} PRIVATE ${generated_headers})
    target_include_directories(${target} PRIVATE
      ${X23_SOURCE_DIR}
      ${X23_BINARY_DIR}
    )
  endif()
endfunction()

set(SUBMODULE_DIRS ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/external)

execute_process(
  COMMAND git rev-parse --show-toplevel
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  RESULT_VARIABLE GIT_TOPLEVEL_RESULT
  OUTPUT_VARIABLE GIT_TOPLEVEL
  ERROR_QUIET
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

if((GIT_TOPLEVEL_RESULT EQUAL 0) AND NOT (GIT_TOPLEVEL EQUAL CMAKE_SOURCE_DIR))
  list(APPEND SUBMODULE_DIRS ${GIT_TOPLEVEL} ${GIT_TOPLEVEL}/external)
endif()

set(GOOGLETEST_PATH "" CACHE PATH "Path to googletest")
set(BENCHMARK_PATH "" CACHE PATH "Path to Google benchmark")

if(NOT GOOGLETEST_PATH)
  foreach(dir IN LISTS SUBMODULE_DIRS)
    if(IS_DIRECTORY ${dir}/googletest)
      set(GOOGLETEST_PATH ${dir}/googletest
        CACHE PATH "Path to googletest" FORCE)
      break()
    endif()
  endforeach()
endif()

if(BUILD_BENCHMARKS AND GOOGLETEST_PATH AND NOT BENCHMARK_PATH)
  foreach(dir IN LISTS SUBMODULE_DIRS)
    if(IS_DIRECTORY ${dir}/benchmark)
      set(BENCHMARK_PATH ${dir}/benchmark
        CACHE PATH "Path to Google benchmark" FORCE)
      break()
    endif()
  endforeach()
endif()

if(X23_IS_TOP_LEVEL)
  if(BENCHMARK_PATH)
    message(STATUS "Enabling benchmark")
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "test benchmark library" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "install benchmark" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "use test benchmark" FORCE)
    set(BENCHMARK_USE_LIBCXX ${USE_LIBCXX} CACHE BOOL "use LLVM libc++" FORCE)
    add_subdirectory(${BENCHMARK_PATH} ${CMAKE_BINARY_DIR}/benchmark)
  endif()
endif()

enable_testing()
add_subdirectory(x23c)
add_subdirectory(x23)
add_subdirectory(tests)

add_custom_target(check
  DEPENDS build_tests
  COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --switch=$(COLOR) --cyan "Running tests..."
  COMMAND ${CMAKE_CTEST_COMMAND} --force-new-ctest-process --output-on-failure --parallel
  COMMENT "Building and running tests"
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/x23Config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/x23Config.cmake"
  INSTALL_DESTINATION lib/cmake/x23
  PATH_VARS CMAKE_INSTALL_PREFIX
)

# Generate the version file for the config file
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/x23ConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

if(X23_IS_TOP_LEVEL)
  # Install the configuration files
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/x23Config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/x23ConfigVersion.cmake"
    DESTINATION lib/cmake/x23
  )
  # Install the export file
  install(EXPORT x23Targets
    FILE x23Targets.cmake
    DESTINATION lib/cmake/x23
  )
endif()
