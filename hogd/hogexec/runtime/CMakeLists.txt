set_source_files_properties(
    ${CMAKE_CURRENT_BINARY_DIR}/syscall_inner.h
    PROPERTIES GENERATED TRUE)

set_source_files_properties(
    ${CMAKE_BINARY_DIR}/hogd/hogexec/wasmi/bindings.h
    PROPERTIES GENERATED TRUE)

add_subdirectory(test_util)

add_library(
  runtime
  database_proxy.cc
  execution_mode.cc
  invocation_frame.cc
  item_view.cc
  native_runtime.cc
  runtime.cc
  status.cc
  storage_snapshot_cache.cc
  syscall_bytestring.cc
  syscall_contract.cc
  syscall_crypto.cc
  syscall_delete.cc
  syscall_entry.cc
  syscall_handler.cc
  syscall_invoke.cc
  syscall_log.cc
  syscall_nnint.cc
  wasmi_runtime.cc
)

add_executable(
  runtime_test
  error_handling_test.cc
  execution_mode_test.cc
  gas_api_test.cc
  instantiate_test.cc
  invoke_arity_test.cc
  item_view_test.cc
  memory_access_test.cc
  native_contract_test.cc
  syscall_entry_outer_tests.cc
  userctx_test.cc
)
target_link_libraries(
  runtime
  wasmi_lib
  runtime_trampoline
  protocol
  hogd
)

add_dependencies(
  runtime
  generate_syscall_inner_h
  generate_wasmi_ffi_h
  protocol
)

target_include_directories(
  runtime
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

target_include_directories(
  runtime_test
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

target_link_libraries(
  runtime_test
  runtime
  runtime_test_util
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
