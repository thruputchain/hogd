add_library(contract_tests_objects OBJECT
    contract_tests_main.cc
    payment_channel_integration_test.cc
    htlc_integration_test.cc
    tuple_return_wasm_test.cc
    native_wasm_token_event_parity_test.cc
)

target_link_libraries(
  contract_tests_objects
  hogd_interface
  GTest::gtest
)

target_include_directories(
  contract_tests_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

add_dependencies(
  contract_tests_objects
  all_wasm_headers
)

add_executable(
  contract_tests
  $<TARGET_OBJECTS:contract_tests_objects>
)

target_link_libraries(
  contract_tests
  precompiles
  runtime
  runtime_test_lib
  runtime_test_util
  contract_loader
  GTest::gtest
)
gtest_discover_tests(contract_tests)
