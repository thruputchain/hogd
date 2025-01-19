add_library(
  interface
  executor_state.cc
  rpc_interface.cc
  executor_state_manager.cc
)

target_link_libraries(
  interface
  protocol
  hogd
  runtime
  wasmi_lib
)

add_executable(
  interface_test
  executor_state_test.cc
)

target_link_libraries(
  interface_test
  interface
  GTest::gtest_main
)

gtest_discover_tests(interface_test)