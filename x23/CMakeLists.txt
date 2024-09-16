cmake_minimum_required(VERSION 3.30)
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
  set(USE_LIBCXX ON)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(X23_IS_TOP_LEVEL TRUE)
endif()

if(X23_IS_TOP_LEVEL)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  if(NOT CMAKE_CXX_FLAGS)
    set(CMAKE_CXX_FLAGS "-g -O2 -Wall" CACHE STRING "Default C++ flags" FORCE)
  endif()
endif()

include(cmake/pause.cmake)
include(cmake/interference_size.cmake)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/x23/config.h
  @ONLY
)

set(X23_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(X23_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

if(NOT X23_IS_TOP_LEVEL)
  set(X23_SOURCE_DIR ${X23_SOURCE_DIR} PARENT_SCOPE)
  set(X23_BINARY_DIR ${X23_BINARY_DIR} PARENT_SCOPE)
endif()

function(use_x23 target)
  get_target_property(target_sources ${target} SOURCES)
  set(generated_headers)

  # Determine the x23c executable
  if(CMAKE_CROSSCOMPILING)
    find_program(X23C_EXECUTABLE x23c REQUIRED)
  else()
    set(X23C_EXECUTABLE $<TARGET_FILE:x23c>)
  endif()

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

set(GOOGLETEST_PATH "" CACHE PATH "Path to googletest")
set(BENCHMARK_PATH "" CACHE PATH "Path to Google benchmark")

if(NOT GOOGLETEST_PATH)
  if(IS_DIRECTORY ${CMAKE_SOURCE_DIR}/googletest)
    set(GOOGLETEST_PATH ${CMAKE_SOURCE_DIR}/googletest
      CACHE PATH "Path to googletest" FORCE)
  endif()
endif()

if(GOOGLETEST_PATH)
  if(NOT BENCHMARK_PATH)
    if(IS_DIRECTORY ${CMAKE_SOURCE_DIR}/benchmark)
      set(BENCHMARK_PATH ${CMAKE_SOURCE_DIR}/benchmark
	CACHE PATH "Path to Google benchmark" FORCE)
    endif()
  endif()
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
  COMMAND ${CMAKE_CTEST_COMMAND} --force-new-ctest-process --output-on-failure
  COMMENT "Building and running tests"
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/x23Config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/x23Config.cmake"
  INSTALL_DESTINATION lib/cmake/x23
  NO_SET_AND_CHECK_MACRO
  NO_CHECK_REQUIRED_COMPONENTS_MACRO
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
    NAMESPACE x23::
    DESTINATION lib/cmake/x23
  )
endif()
