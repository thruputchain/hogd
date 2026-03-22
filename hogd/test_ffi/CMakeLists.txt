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

# Statically link libc++ so consumers don't need a specific version
# installed. Derive the lib directory from the compiler path (works with
# Homebrew LLVM on macOS and apt-installed LLVM on Linux).
if(ENABLE_TEST_FFI)
  get_filename_component(_cxx_real "${CMAKE_CXX_COMPILER}" REALPATH)
  get_filename_component(_cxx_bin_dir "${_cxx_real}" DIRECTORY)
  get_filename_component(_cxx_prefix "${_cxx_bin_dir}" DIRECTORY)
  set(_libcxx_a "${_cxx_prefix}/lib/libc++.a")
  set(_libcxxabi_a "${_cxx_prefix}/lib/libc++abi.a")
  if(EXISTS "${_libcxx_a}" AND EXISTS "${_libcxxabi_a}")
    target_link_options(test_ffi PRIVATE -nostdlib++)
    target_link_libraries(test_ffi "${_libcxx_a}" "${_libcxxabi_a}")
  else()
    message(FATAL_ERROR "Static libc++ not found at ${_cxx_prefix}/lib")
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



