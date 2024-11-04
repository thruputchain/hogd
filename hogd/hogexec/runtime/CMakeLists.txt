set_source_files_properties(
    ${CMAKE_CURRENT_BINARY_DIR}/syscall_entry.cc
    PROPERTIES GENERATED TRUE)

set_source_files_properties(
    ${CMAKE_CURRENT_BINARY_DIR}/syscall_inner.h
    PROPERTIES GENERATED TRUE)

set(runtime_sources 
  auth_entry_matcher.cc
  call_frame.cc
  status.cc
  item_cache.cc
  storage_snapshot_cache.cc
  syscall_bytestring.cc
  syscall_contract.cc
  syscall_crypto.cc
  ${CMAKE_CURRENT_BINARY_DIR}/syscall_entry.cc
  syscall_general_storage.cc
  syscall_invoke.cc
  syscall_handler.cc
  syscall_log.cc
  syscall_nnint64.cc
  syscall_replay.cc
)

add_library(
  runtime
  ${runtime_sources}
  wasmi_runtime.cc
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
  runtime
  wasmi_lib
  hogd
  protocol
  softstate
  ${LIBSODIUM_LIBRARIES}
)

add_executable(
  runtime_test
  runtime_test_main.cc
  userctx_test.cc
  instantiate_test.cc
  error_handling_test.cc
  invoke_arity_test.cc
  gas_api_test.cc
  memory_access_test.cc
  item_cache_test.cc
  calldata_test.cc
  invoke_test.cc
  return_test.cc
  bytestring_test.cc
  nnint64_test.cc
  deployer_test.cc
  erc20_test.cc
  erc20_test_utils.cc
  auth_match_test.cc
)

target_link_libraries(
  runtime
  wasmi_lib
  protocol
)
target_link_libraries(
  runtime_test
  runtime
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

add_dependencies(
  runtime_test
  test_invoke_wasm
  test_error_handling_wasm
  test_invoke_arity_wasm
  test_set_memory_wasm
  rs_test_contracts
)

# create crypto module benchmark binary
add_executable(
  erc20_bench
  erc20_bench.cc
  erc20_test_utils.cc
)
target_link_libraries(
  erc20_bench
  runtime
  hogd
  protocol
  ${LIBSODIUM_LIBRARIES}
  benchmark::benchmark
)
add_dependencies(erc20_bench libsodium)
