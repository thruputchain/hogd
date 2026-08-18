cmake_minimum_required(VERSION 3.18)
project(hogd VERSION 0.1.2 LANGUAGES C CXX)

set(CMAKE_POLICY_VERSION_MINIMUM 3.18)

# Configuration parameters
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_COLOR_MAKEFILE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)

# Configuration options
option(BUILD_FUZZERS "Build fuzz targets" OFF)
# Enable building test ffi (default: OFF)
# Turning test ffi on requires building with fPIC
option(ENABLE_TEST_FFI "Enable mock contracts & test FFI" OFF)
option(STATIC_LIBCXX "Statically link libc++ into test_ffi" OFF)
set(BUILD_ARCHITECTURE "native" CACHE STRING "Architecture to build for (default 'native')")
set(RUST_BUILD_CPU "native" CACHE STRING "Rust target CPU to build for (default 'native')")

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
#if(NOT CMAKE_BUILD_TYPE)
#  set(CMAKE_BUILD_TYPE Release)
#endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
if(ENABLE_TEST_FFI)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Set up compiler flags for release and debug build types
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -march=${BUILD_ARCHITECTURE}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -march=${BUILD_ARCHITECTURE}")
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_compile_options(-stdlib=libc++ -fexperimental-library)
  add_link_options(-stdlib=libc++ -fexperimental-library)
  add_link_options(-fuse-ld=lld)
  # Optional but robust when archiving bitcode:
  set(CMAKE_AR llvm-ar)
  set(CMAKE_RANLIB llvm-ranlib)
endif()
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-interference-size")
  link_libraries(stdc++exp) # needed for C++23 stacktrace library
  link_libraries(atomic) # needed for std::atomic<int128_t>
endif()

message(STATUS "Using CMAKE_C_FLAGS: ${CMAKE_C_FLAGS}")
message(STATUS "Using CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")

set(CMAKE_C_FLAGS_DEBUG "-DDEBUG -O0 -g")
set(CMAKE_C_FLAGS_RELEASE "-DNDEBUG -O3")
set(CMAKE_CXX_FLAGS_DEBUG "-DDEBUG -O0 -g")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG -O3")

message(STATUS "Using CMAKE_C_FLAGS_DEBUG: ${CMAKE_C_FLAGS_DEBUG}")
message(STATUS "Using CMAKE_C_FLAGS_RELEASE: ${CMAKE_C_FLAGS_RELEASE}")
message(STATUS "Using CMAKE_CXX_FLAGS_DEBUG: ${CMAKE_CXX_FLAGS_DEBUG}")
message(STATUS "Using CMAKE_CXX_FLAGS_RELEASE: ${CMAKE_CXX_FLAGS_RELEASE}")

include(cmake/stacktrace.cmake)

# Where to place generated headers
set(BUILT_HEADERS_DIR "${CMAKE_CURRENT_BINARY_DIR}/built_headers")
file(MAKE_DIRECTORY "${BUILT_HEADERS_DIR}")

set(HOGD_BUILT_HEADERS_DIR "${BUILT_HEADERS_DIR}/hogd")
file(MAKE_DIRECTORY "${HOGD_BUILT_HEADERS_DIR}")

include(cmake/repro.cmake)

generate_cmake_repro_string(CMAKE_REPRO_COMMAND)

# === Interface target that exposes the header and enforces the dep ===
add_library(built_headers_interface INTERFACE)
# Only this target provides the include dir where the header lives:
target_include_directories(built_headers_interface INTERFACE "${BUILT_HEADERS_DIR}")

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

# When enabled, test commands are prefixed with a wrapper that captures gdb
# backtraces on timeout (instead of ctest's silent SIGKILL).
option(CRASHDUMP_ON_TIMEOUT
  "Wrap test commands with gdb-backtrace-on-timeout for diagnostics" OFF)
if(CRASHDUMP_ON_TIMEOUT)
  set(CRASHDUMP_WRAPPER "${CMAKE_SOURCE_DIR}/scripts/ci-crashdump-wrapper.sh")
endif()

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
# BENCHMARK_ENABLE_TESTING enables the tests on the google benchmark library.
option(BENCHMARK_ENABLE_TESTING "" OFF)
# Disable -Werror inside benchmark — newer Clang emits -Wc2y-extensions for
# __COUNTER__ which is outside our control.
option(BENCHMARK_ENABLE_WERROR "" OFF)

# BUILD_BENCHMARKS turns on or off the hogd benchmarks
option(BUILD_BENCHMARKS "" ON)

if(BUILD_BENCHMARKS)
add_subdirectory(external/benchmark)
# Suppress -Wc2y-extensions from Clang ≥22 on benchmark.h's __COUNTER__ usage.
target_compile_options(benchmark PRIVATE -Wno-c2y-extensions)
target_compile_options(benchmark_main PRIVATE -Wno-c2y-extensions)
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
  set(HOGD_RULE_LAUNCH_COMPILE "${CMAKE_SOURCE_DIR}/cmake/fail_wrapper.sh ${ENABLE_CI_FAIL_LOG}")
  set(HOGD_RULE_LAUNCH_LINK "${CMAKE_SOURCE_DIR}/cmake/fail_wrapper.sh ${ENABLE_CI_FAIL_LOG}")
else()
  set(HOGD_RULE_LAUNCH_COMPILE "")
  set(HOGD_RULE_LAUNCH_LINK "")
endif()


# Enable x23
set(X23_MAKE_ALL_DEPEND_ON_TESTS ON)
add_subdirectory(x23)
include_directories(${X23_SOURCE_DIR} ${X23_BINARY_DIR})

option(RUST_TSAN
    "Build Rust crates with ThreadSanitizer instrumentation. Requires \
the nightly toolchain and the rust-src component. Pair with a C++ build \
that has -fsanitize=thread; libtsan is shared across the two."
    OFF)

# Enable Corrosion
# The rust toolchain must be set FIRST!
#
# -Zsanitizer=thread and -Zbuild-std (both needed to make Rust visible
# to TSAN) are nightly-only, so RUST_TSAN forces the toolchain to
# nightly here. Corrosion derives the cargo target triple from whatever
# rustc resolves under this toolchain, so the target follows the host
# architecture automatically.
if(RUST_TSAN)
    set(Rust_TOOLCHAIN "nightly" CACHE STRING "Rust toolchain to use" FORCE)
    message(STATUS "RUST_TSAN=ON: forcing Rust_TOOLCHAIN=nightly")
endif()
set(Rust_TOOLCHAIN "1.93.0" CACHE STRING "Rust toolchain to use")
add_subdirectory(external/corrosion)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/corrosion/cmake")
include(Corrosion)
message(STATUS "Rust version: ${Rust_VERSION}")

# Enable model-checker
add_subdirectory(external/model-checker)
set(MODEL_CHECKER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/model-checker")
include_directories(${MODEL_CHECKER_SOURCE_DIR})

# Enable rocksdb
# Disable tests and tools
set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_GTEST OFF CACHE BOOL "" FORCE)
set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_CORE_TOOLS OFF CACHE BOOL "" FORCE)
set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(FAIL_ON_WARNINGS OFF CACHE BOOL "" FORCE)
# Performance optimizations
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(USE_RTTI OFF CACHE BOOL "" FORCE)
else()
  # Required in debug builds: rocksdb uses dynamic_cast in assertions.
  set(USE_RTTI ON CACHE BOOL "" FORCE)
endif()

# Keep RocksDB on the same portable CPU baseline as the rest of hogd. RocksDB's
# PORTABLE=OFF path injects -march=native, which can leak AVX-512 into release
# artifacts built on AVX-512-capable runners.
set(PORTABLE ON CACHE BOOL "" FORCE)
set(FORCE_SSE42 ON CACHE BOOL "" FORCE)  # Enable SSE4.2 (modern CPUs)
# Compression support (all fast and commonly available)
set(WITH_SNAPPY ON CACHE BOOL "" FORCE)
set(WITH_LZ4 ON CACHE BOOL "" FORCE)
set(WITH_ZSTD ON CACHE BOOL "" FORCE)
set(WITH_ZLIB ON CACHE BOOL "" FORCE)
set(WITH_BZ2 ON CACHE BOOL "" FORCE)
# Set variables for config.h
set(ROCKSDB_WITH_SNAPPY ${WITH_SNAPPY})
set(ROCKSDB_WITH_LZ4 ${WITH_LZ4})
set(ROCKSDB_WITH_ZSTD ${WITH_ZSTD})
set(ROCKSDB_WITH_ZLIB ${WITH_ZLIB})
set(ROCKSDB_WITH_BZ2 ${WITH_BZ2})
message(STATUS "ROCKSDB_WITH_SNAPPY: ${ROCKSDB_WITH_SNAPPY}")
message(STATUS "ROCKSDB_WITH_LZ4: ${ROCKSDB_WITH_LZ4}")
message(STATUS "ROCKSDB_WITH_ZSTD: ${ROCKSDB_WITH_ZSTD}")
message(STATUS "ROCKSDB_WITH_ZLIB: ${ROCKSDB_WITH_ZLIB}")
message(STATUS "ROCKSDB_WITH_BZ2: ${ROCKSDB_WITH_BZ2}")
# Memory allocator (jemalloc is faster than system malloc)
set(WITH_JEMALLOC ON CACHE BOOL "Build RocksDB with jemalloc")
set(HOGD_SANITIZER_FLAGS
    "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS} ${CMAKE_MODULE_LINKER_FLAGS}")
string(FIND "${HOGD_SANITIZER_FLAGS}" "-fsanitize=address" HOGD_ASAN_FLAG_POS)
if(WITH_JEMALLOC AND NOT HOGD_ASAN_FLAG_POS EQUAL -1)
  message(WARNING
      "WITH_JEMALLOC=ON with -fsanitize=address may cause AddressSanitizer "
      "violations during process initialization. Consider -DWITH_JEMALLOC=OFF.")
endif()
# Fix for macOS/LLVM: atomic operations are built-in, no need for -latomic
set(BUILTIN_ATOMIC ON CACHE BOOL "" FORCE)
add_subdirectory(external/rocksdb EXCLUDE_FROM_ALL)
# RocksDB's CMakeLists globally installs ccache as RULE_LAUNCH_* when ccache is
# present. The wrapper already configures ccache through CMAKE_*_COMPILER_LAUNCHER,
# and leaving both enabled generates nested `ccache ccache ...` Ninja commands.
set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${HOGD_RULE_LAUNCH_COMPILE}")
set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK "${HOGD_RULE_LAUNCH_LINK}")
set(ROCKSDB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/rocksdb/include")
include_directories(${ROCKSDB_SOURCE_DIR})

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.in
  ${HOGD_BUILT_HEADERS_DIR}/config.h
  @ONLY
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_config.h.in
  ${HOGD_BUILT_HEADERS_DIR}/build_config.h
  @ONLY
)

add_subdirectory(built_contracts)

# Finally, enable hogd's own code
include_directories(${CMAKE_SOURCE_DIR})

# Set after externals so try_compile/try_run checks are unaffected.
# CMAKE_CROSSCOMPILING_EMULATOR initializes CROSSCOMPILING_EMULATOR on new
# targets, which gtest_discover_tests uses to prefix test commands.
if(CRASHDUMP_ON_TIMEOUT)
  message(STATUS "CRASHDUMP_ON_TIMEOUT: wrapping hogd tests with ${CRASHDUMP_WRAPPER}")
  set(CMAKE_CROSSCOMPILING ON)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${CRASHDUMP_WRAPPER}")
endif()

add_subdirectory(hogd)

if(BUILD_BENCHMARKS)
  # Aggregate target: build and run every in-process (server-free) benchmark in
  # sequence -- `cmake --build build --target local_benchmarks`. These need no
  # bench_server / coordinator, unlike payments_bench / fibonacci_*only.
  add_custom_target(local_benchmarks
    COMMAND $<TARGET_FILE:rv32_vm_bench>
    COMMAND $<TARGET_FILE:payments_exec_bench>
    COMMAND $<TARGET_FILE:erc20_experiment_bench>
    USES_TERMINAL
    VERBATIM
    COMMENT "Running all local (in-process) benchmarks")
  add_dependencies(local_benchmarks
    rv32_vm_bench
    payments_exec_bench
    erc20_experiment_bench)
endif()
