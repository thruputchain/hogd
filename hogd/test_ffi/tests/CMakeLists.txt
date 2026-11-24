# mock_syscall_test.cc drives internal C++ APIs directly (constructing
# SyscallHandlerContext / mock_syscall_parser without going through an extern
# "C" entry point), so it never reaches lazy_init() and needs logging and the
# executor metrics initialized up front. It therefore keeps its own custom
# main() (run_hog_exec_tests, via GTest::gtest with no gtest_main) and lives in
# its own binary. Keeping it separate lets test_ffi_test below rely solely on
# lazy_init() -- the real initialization path FFI consumers take -- so a
# subsystem an entry point forgets to initialize surfaces as a test failure
# there instead of being masked by a test-only main.
add_executable(
  test_ffi_mock_syscall_test
  mock_syscall_test.cc
)

target_include_directories(
  test_ffi_mock_syscall_test
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

target_link_libraries(
  test_ffi_mock_syscall_test
  test_ffi_objects
  auth_claim
  precompiles
  runtime
  hog_auth
  runtime_test_util
  GTest::gtest
)
hogd_discover_tests(test_ffi_mock_syscall_test)

# These tests exercise the public extern "C" API, so they go through
# lazy_init() on first use exactly as a real consumer does. They link the
# default GTest::gtest_main (no custom init), so nothing is initialized ahead
# of the first API call: an entry point that fails to initialize a subsystem
# fails these tests rather than silently relying on a test-only main.
#
# Per-issue security-regression tests live in their own *_regression_test.cc
# files and are picked up by this glob, so adding one never edits a shared file
# (which previously caused merge conflicts between in-flight test PRs). The
# shared setup helpers they use live in test_ffi_test_helpers.h. CONFIGURE_DEPENDS
# makes the build re-glob when files are added or removed.
file(GLOB TEST_FFI_REGRESSION_TESTS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*_regression_test.cc")
add_executable(
  test_ffi_test
  test_bindings_test.cc
  rv32_stack_usage_test.cc
  ${TEST_FFI_REGRESSION_TESTS}
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
  hog_auth
  runtime_test_util
  GTest::gtest
  GTest::gtest_main
)
hogd_discover_tests(test_ffi_test)

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
  hog_auth
  runtime_test_util
  GTest::gtest
  GTest::gtest_main
)
hogd_discover_tests(test_ffi_first_call_test)
