add_library(
  runtime
  wasmi_runtime.cc
  ffi_trampolines.cc
  host_fn_status.cc
)
add_executable(
  runtime_test
  userctx_text.cc
)
target_link_libraries(
  runtime_test
  runtime
  wasmi_lib
  GTest::gtest_main
)
gtest_discover_tests(runtime_test)
<<<<<<< HEAD

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

add_dependencies(
  runtime_test
  test_call_simple_wasm
  test_invoke_wasm
)
=======
>>>>>>> ff18635 (initial draft wasmi runtime no invoke() or wat2wasm yet)
