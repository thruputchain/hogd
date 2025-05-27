add_library(
    preset_contracts
    shared.cc
    native_token.cc
)

target_link_libraries(
  preset_contracts
  runtime
  protocol
  hogd
)

add_dependencies(
  preset_contracts
  protocol
)

add_executable(
  preset_contracts_test
  shared_test.cc
)

target_link_libraries(
  preset_contracts_test
  preset_contracts
  hogd
  GTest::gtest
)

gtest_discover_tests(preset_contracts_test)

