add_library(wasm_runtime_objects OBJECT
  wasmi_runtime.cc
  wasmtime_runtime.cc
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # Match the runtime_objects warning threshold. This is an arbitrary
  # Release-only tripwire, not a hard correctness limit.
  target_compile_options(wasm_runtime_objects PRIVATE
    $<$<CONFIG:Release>:-Wframe-larger-than=1536>)
endif()

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

add_dependencies(
  wasm_runtime_test_objects
  generate_wasm_meter_ffi_h
)

target_include_directories(
  wasm_runtime_test_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

add_library(wasm_meter_objects OBJECT
  instrumented_bytes.cc
)

target_link_libraries(
  wasm_meter_objects
  hogd_interface
)

add_dependencies(
  wasm_meter_objects
  generate_wasm_meter_ffi_h
)

target_include_directories(
  wasm_meter_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

add_executable(
  wasm_runtime_test
  wasm_runtime_test_main.cc
  $<TARGET_OBJECTS:wasm_runtime_test_objects>
  $<TARGET_OBJECTS:wasm_meter_objects>
)

target_link_libraries(
  wasm_runtime_test
  runtime
  runtime_test_util
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

wat2wasm_target(
  test_oob_data_segment_wasm
  wat/test_oob_data_segment.wat
  ${CMAKE_CURRENT_BINARY_DIR}/test_oob_data_segment.wasm
)

wat2wasm_target(
  test_oob_element_segment_wasm
  wat/test_oob_element_segment.wat
  ${CMAKE_CURRENT_BINARY_DIR}/test_oob_element_segment.wasm
)

wat2wasm_target(
  test_large_memory_wasm
  wat/test_large_memory.wat
  ${CMAKE_CURRENT_BINARY_DIR}/test_large_memory.wasm
)

wat2wasm_target(
  test_wrong_import_type_wasm
  wat/test_wrong_import_type.wat
  ${CMAKE_CURRENT_BINARY_DIR}/test_wrong_import_type.wasm
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
  test_oob_data_segment_wasm
  test_oob_element_segment_wasm
  test_wrong_import_type_wasm
  all_wasm_contracts
)
