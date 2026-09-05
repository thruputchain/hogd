add_library(contract_tests_objects OBJECT
    contract_tests_main.cc
    payment_channel_integration_test.cc
    htlc_integration_test.cc
    tuple_return_rv32_test.cc
    native_rv32_token_event_parity_test.cc
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
  all_contract_headers
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
add_dependencies(
  contract_tests
  all_riscvs_built
)

gtest_discover_tests(contract_tests)
