add_library(
    test_ffi
    SHARED
    db_context_wrapper.cc
    logger.cc
    mock_contract.cc
    mock_runtime.cc
    syscall_handler_wrapper.cc
    test_bindings.cc
)

add_dependencies(
  test_ffi
  generate_syscall_meta_h
  generate_syscall_inner_h
)

target_include_directories(
  test_ffi
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

target_link_libraries(
  test_ffi
  auth_claim
  precompiles
  runtime
  auth
  runtime_test_util
)

# STATIC_LIBCXX option is declared in the top-level CMakeLists.txt so that
# -fno-typed-cxx-new-delete can be set globally before any targets are defined.
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
      if(EXISTS "${_libcxx_a}")
        target_link_options(test_ffi PRIVATE -nostdlib++)
        # Only statically link libc++ (the standard library). libc++abi has a
        # stable ABI and must remain dynamic to avoid a TMO static initializer
        # ordering crash in Clang 22+.
        target_link_libraries(test_ffi "${_libcxx_a}" -lc++abi)
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

# Set rpath so test_ffi can find the Rust cdylib in the same directory.
# Windows doesn't need this - it searches the DLL's directory by default.
if(APPLE)
  set_target_properties(test_ffi PROPERTIES
    BUILD_RPATH "@loader_path"
    INSTALL_RPATH "@loader_path"
  )
elseif(UNIX)
  set_target_properties(test_ffi PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
  )
endif()

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
  test_ffi
  GTest::gtest
)
gtest_discover_tests(test_ffi_test)



