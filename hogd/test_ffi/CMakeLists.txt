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



