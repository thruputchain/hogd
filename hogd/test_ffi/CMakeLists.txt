set_source_files_properties(
    ${CMAKE_CURRENT_BINARY_DIR}/mock_syscall_entry.cc
    PROPERTIES GENERATED TRUE)

add_library(
    test_ffi
    SHARED
    test_bindings.cc
    syscall_handler_wrapper.cc
    db_context_wrapper.cc
    mock_syscall_entry.cc
)

add_dependencies(
  test_ffi
  generate_mock_syscall_entry_cc
)

target_include_directories(
  test_ffi
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

target_link_libraries(
  test_ffi
  hogdb_core
  runtime
)

add_executable(
  test_ffi_test
  mock_syscall_test.cc
)

target_link_libraries(
  test_ffi_test
  test_ffi
  GTest::gtest_main
)
gtest_discover_tests(test_ffi_test)



