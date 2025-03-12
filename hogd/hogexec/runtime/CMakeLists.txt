set_source_files_properties(
    ${CMAKE_CURRENT_BINARY_DIR}/syscall_entry.cc
    PROPERTIES GENERATED TRUE)

set_source_files_properties(
    ${CMAKE_CURRENT_BINARY_DIR}/syscall_inner.h
    PROPERTIES GENERATED TRUE)

add_library(
  runtime
  wasmi_runtime.cc
  status.cc
  syscall_crypto.cc
  syscall_entry_foo.cc
  ${CMAKE_CURRENT_BINARY_DIR}/syscall_entry.cc
)

add_executable(
  runtime_test
  userctx_test.cc
  instantiate_test.cc
  error_handling_test.cc
  invoke_arity_test.cc
  gas_api_test.cc
  memory_access_test.cc
)
target_link_libraries(
  runtime
  wasmi_lib
  runtime_trampoline
  protocol
  crypto
)

add_dependencies(
  runtime
  generate_syscall_inner_h
  generate_syscall_entry_cc
  protocol
)

target_include_directories(
  runtime
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

target_link_libraries(
  runtime_test
  runtime
  GTest::gtest_main
)
gtest_discover_tests(runtime_test)

wat2wasm_target(
  test_invoke_wasm
  wat/test_invoke.wat
  ${CMAKE_CURRENT_BINARY_DIR}/test_invoke.wasm
)

wat2wasm_target(
  test_error_handling_wasm
  wat/test_error_handling.wat
  ${CMAKE_CURRENT_BINARY_DIR}/test_error_handling.wasm
)

wat2wasm_target(
  test_invoke_arity_wasm
  wat/test_invoke_arity.wat
  ${CMAKE_CURRENT_BINARY_DIR}/test_invoke_arity.wasm
)

wat2wasm_target(
  test_set_memory_wasm
  wat/test_set_memory.wat
  ${CMAKE_CURRENT_BINARY_DIR}/test_set_memory.wasm
)

add_dependencies(
  runtime_test
  test_invoke_wasm
  test_error_handling_wasm
  test_invoke_arity_wasm
  test_set_memory_wasm
)
