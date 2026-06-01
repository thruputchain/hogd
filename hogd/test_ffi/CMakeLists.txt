# Object library shared between the dylib and the test executable.
add_library(
    test_ffi_objects
    OBJECT
    db_context_wrapper.cc
    logger.cc
    mock_contract.cc
    mock_runtime.cc
    syscall_handler_wrapper.cc
    test_bindings.cc
)

add_dependencies(
  test_ffi_objects
  generate_syscall_meta_h
  generate_syscall_inner_h
)

target_include_directories(
  test_ffi_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

# Propagate compile definitions and include directories from link deps.
target_link_libraries(
  test_ffi_objects
  auth_claim
  precompiles
  runtime
  auth
  runtime_test_util
)

# Hide all C++ symbols; only the extern "C" API in test_bindings.h is public.
# For the test executable (which also links these objects) visibility is
# irrelevant — executables don't have a symbol export table.
set_target_properties(test_ffi_objects PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

# The shared library is consumed by Rust over the C API.
add_library(test_ffi SHARED)
target_link_libraries(
  test_ffi
  test_ffi_objects
  auth_claim
  precompiles
  runtime
  auth
  runtime_test_util
)

# STATIC_LIBCXX option is declared in the top-level CMakeLists.txt.
message(STATUS "STATIC_LIBCXX: ${STATIC_LIBCXX}")
if(STATIC_LIBCXX)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
      "STATIC_LIBCXX requires Clang. Current compiler: ${CMAKE_CXX_COMPILER_ID}")
  endif()

  # Try the unresolved compiler path first (Homebrew symlinks point to the
  # right lib dir, but REALPATH lands in the Cellar where static libs aren't
  # installed).  Fall back to the resolved path for Linux where
  # /usr/bin/clang++-21 -> /usr/lib/llvm-21/bin/clang++.
  set(_candidate_prefixes "")
  get_filename_component(_cxx_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(_cxx_prefix "${_cxx_bin_dir}" DIRECTORY)
  list(APPEND _candidate_prefixes "${_cxx_prefix}")
  get_filename_component(_cxx_real "${CMAKE_CXX_COMPILER}" REALPATH)
  get_filename_component(_cxx_real_bin "${_cxx_real}" DIRECTORY)
  get_filename_component(_cxx_real_prefix "${_cxx_real_bin}" DIRECTORY)
  list(APPEND _candidate_prefixes "${_cxx_real_prefix}")
  list(REMOVE_DUPLICATES _candidate_prefixes)

  set(_found_libcxx FALSE)
  foreach(_prefix IN LISTS _candidate_prefixes)
    # Homebrew LLVM puts libc++ in lib/c++/; apt-installed LLVM uses lib/.
    foreach(_libdir "${_prefix}/lib/c++" "${_prefix}/lib")
      set(_libcxx_a "${_libdir}/libc++.a")
      set(_libcxxabi_a "${_libdir}/libc++abi.a")
      if(EXISTS "${_libcxx_a}" AND EXISTS "${_libcxxabi_a}")
        target_link_options(test_ffi PRIVATE -nostdlib++)
        target_link_libraries(test_ffi "${_libcxx_a}" "${_libcxxabi_a}")
        # Homebrew LLVM builds libc++abi against its own unwinder; pull it in
        # if present so we don't leave _Unwind_* symbols unresolved.
        set(_libunwind_a "${_libdir}/libunwind.a")
        if(EXISTS "${_libunwind_a}")
          target_link_libraries(test_ffi "${_libunwind_a}")
        endif()
        set(_found_libcxx TRUE)
        break()
      endif()
    endforeach()
    if(_found_libcxx)
      break()
    endif()
  endforeach()
  if(NOT _found_libcxx)
    message(FATAL_ERROR "Static libc++ not found in any of: ${_candidate_prefixes}")
  endif()
endif()

# Strip non-API symbols from the dylib.  On macOS, lld-macho lacks
# --exclude-libs so we use an explicit exported-symbols list.  On Linux,
# --exclude-libs,ALL hides symbols pulled from static archives (libc++,
# libc++abi, libunwind, etc.); our own C++ symbols are already hidden via
# CXX_VISIBILITY_PRESET above, and the extern "C" API retains default
# visibility via a pragma in test_bindings.cc.
if(APPLE)
  target_link_options(test_ffi PRIVATE
    "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/test_ffi.exports")
elseif(UNIX)
  target_link_options(test_ffi PRIVATE "LINKER:--exclude-libs,ALL")
endif()

# Tests link against the object files directly so they can reach internal
# C++ symbols that the dylib intentionally hides.
add_executable(
  test_ffi_test
  mock_syscall_test.cc
  test_bindings_test.cc
)

target_include_directories(
  test_ffi_test
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

target_link_libraries(
  test_ffi_test
  test_ffi_objects
  auth_claim
  precompiles
  runtime
  auth
  runtime_test_util
  GTest::gtest
)
add_dependencies(
  test_ffi_test
  test_large_memory_wasm
)
gtest_discover_tests(test_ffi_test)

# Separate binary for coverage that must run before logging is initialized.
# It links GTest::gtest_main (the default main) instead of the custom main()
# in mock_syscall_test.cc, which calls hogd_init() before tests run and would
# otherwise mask the uninitialized-logger path exercised here.
add_executable(
  test_ffi_first_call_test
  test_bindings_first_call_test.cc
)

target_include_directories(
  test_ffi_first_call_test
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

target_link_libraries(
  test_ffi_first_call_test
  test_ffi_objects
  auth_claim
  precompiles
  runtime
  auth
  runtime_test_util
  GTest::gtest
  GTest::gtest_main
)
gtest_discover_tests(test_ffi_first_call_test)
