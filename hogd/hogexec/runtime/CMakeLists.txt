set_source_files_properties(
    ${CMAKE_CURRENT_BINARY_DIR}/syscall_inner.h
    PROPERTIES GENERATED TRUE)

set_source_files_properties(
    ${CMAKE_CURRENT_BINARY_DIR}/syscall_meta.h
    PROPERTIES GENERATED TRUE)

set_source_files_properties(
    ${CMAKE_BINARY_DIR}/hogd/hogexec/wasmi/bindings.h
    PROPERTIES GENERATED TRUE)

add_subdirectory(test_util)

add_library(runtime_objects OBJECT
  contract_trait.cc
  database_proxy.cc
  execution_mode.cc
  invocation_frame.cc
  item_view.cc
  native_runtime.cc
  multiplexed_context.cc
  preset_native_token.cc
  runtime.cc
  status.cc
  storage_snapshot_cache.cc
  snapshot_storage_item.cc
  syscall_bytestring.cc
  syscall_contract.cc
  syscall_crypto.cc
  syscall_delete.cc
  syscall_entry.cc
  syscall_handler.cc
  syscall_invoke.cc
  syscall_log.cc
  syscall_nnint.cc
  syscall_rcq.cc
  syscall_stats.cc
  transaction_stats.cc
  wasmi_runtime.cc
)

target_link_libraries(
  runtime_objects
  hogd_interface
)

add_library(runtime STATIC $<TARGET_OBJECTS:runtime_objects>)

add_library(runtime_test_objects OBJECT
  contract_trait_test.cc
  cross_call_tests.cc
  cross_contract_revert_test.cc
  direct_bytestring_test.cc
  direct_nnint_test.cc
  error_handling_test.cc
  execution_mode_test.cc
  fibonacci_test.cc
  gas_api_test.cc
  interleaving_tests.cc
  instantiate_test.cc
  invoke_arity_test.cc
  invoke_error_handling_test.cc
  item_view_test.cc
  manual_revert_comprehensive_test.cc
  manual_revert_wasm_test.cc
  manual_revert_caller_wasm_test.cc
  memory_access_test.cc
  native_contract_test.cc
  native_contract_test_framework.cc
  nnint_add_test.cc
  rcq_test.cc
  read_only_test.cc
  revert_tests.cc
  runtime_test.cc
  serialize_roundtrip_test.cc
  snapshot_storage_item_test.cc
  syscall_entry_outer_tests.cc
  syscall_parse_tests.cc
  try_invoke_test.cc
  userctx_test.cc
)

target_link_libraries(
  runtime_test_objects
  GTest::gtest
  hogd_interface
)

add_executable(
  runtime_test
  $<TARGET_OBJECTS:runtime_test_objects>
)

target_link_libraries(
  runtime
  wasmi_lib
  runtime_trampoline
  protocol
  rpc
  hogd
)

add_dependencies(
  runtime_objects
  generate_syscall_inner_h
  generate_syscall_meta_h
  generate_wasmi_ffi_h
  protocol
)

target_include_directories(
  runtime_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

target_include_directories(
  runtime_test_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

target_link_libraries(
  runtime_test
  runtime
  runtime_test_util
  db_interface
  hogdb
  storage_cost
  model_checker
  GTest::gtest
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

wat2wasm_target(
  test_instantiate_fail_wasm
  wat/test_instantiate_fail.wat
  ${CMAKE_CURRENT_BINARY_DIR}/test_instantiate_fail.wasm
)

wat2wasm_target(
  test_instantiate_unexported_memory_wasm
  wat/test_instantiate_unexported_memory.wat
  ${CMAKE_CURRENT_BINARY_DIR}/test_instantiate_unexported_memory.wasm
)

add_dependencies(
  runtime_test_objects
  all_wasm_headers
)

add_dependencies(
  runtime_test
  test_invoke_wasm
  test_error_handling_wasm
  test_invoke_arity_wasm
  test_set_memory_wasm
  test_instantiate_fail_wasm
  test_instantiate_unexported_memory_wasm
  all_wasm_contracts
)
