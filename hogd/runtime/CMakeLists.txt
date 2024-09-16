add_library(
  runtime
  wasmi_runtime.cc
  ffi_trampolines.cc
  status.cc
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
  runtime_test
  runtime
  wasmi_lib
  GTest::gtest_main
)
gtest_discover_tests(runtime_test)

include(../cmake/wat2wasm.cmake)

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
  wasmi_lib
)