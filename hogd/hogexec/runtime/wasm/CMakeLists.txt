add_library(wasm_runtime_objects OBJECT
  wasmi_runtime.cc
  wasmtime_runtime.cc
)

target_link_libraries(
  wasm_runtime_objects
  hogd_interface
)

add_dependencies(
  wasm_runtime_objects
  generate_wasmi_ffi_h
  generate_wasmtime_ffi_h
)

target_include_directories(
  wasm_runtime_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

add_library(wasm_runtime_test_objects OBJECT
  instantiate_test.cc
  invoke_arity_test.cc
  error_handling_test.cc
  gas_api_test.cc
  memory_access_test.cc
  userctx_test.cc
)

target_link_libraries(
  wasm_runtime_test_objects
  GTest::gtest
  hogd_interface
)

target_include_directories(
  wasm_runtime_test_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

add_executable(
  wasm_runtime_test
  wasm_runtime_test_main.cc
  $<TARGET_OBJECTS:wasm_runtime_test_objects>
)

target_link_libraries(
  wasm_runtime_test
  precompiles
  runtime
  runtime_test_lib
  runtime_test_util
  db_interface
  hogdb
  storage_cost
  model_checker
  contract_loader
  genesis
  GTest::gtest
)
gtest_discover_tests(wasm_runtime_test)

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

wat2wasm_target(
  test_no_start_wasm
  wat/test_no_start.wat
  ${CMAKE_CURRENT_BINARY_DIR}/test_no_start.wasm
)

add_dependencies(
  wasm_runtime_test_objects
  all_wasm_headers
)

add_dependencies(
  wasm_runtime_test
  test_invoke_wasm
  test_error_handling_wasm
  test_invoke_arity_wasm
  test_set_memory_wasm
  test_instantiate_fail_wasm
  test_instantiate_unexported_memory_wasm
  test_no_start_wasm
  all_wasm_contracts
)
