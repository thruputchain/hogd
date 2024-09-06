add_library(
  runtime
  wasmi_runtime.cc
  ffi_trampolines.cc
  host_fn_status.cc
)
add_executable(
  runtime_test
  userctx_test.cc
  instantiate_test.cc
  error_handling_test.cc
  invoke_arity_test.cc
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
  test_call_simple_wasm
  wat/test_call_simple.wat
  ${CMAKE_CURRENT_BINARY_DIR}/test_call_simple.wasm
)

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

add_dependencies(
  runtime_test
  test_call_simple_wasm
  test_invoke_wasm
  test_error_handling_wasm
  test_invoke_arity_wasm
)