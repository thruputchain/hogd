add_library(exec_interface_objects OBJECT
  executor.cc
  executor_context.cc
  executor_state_manager.cc
  rpc_from_coord.cc
  rpc_from_view_gateway.cc
)
target_link_libraries(
  exec_interface_objects
  hogd_interface
)

add_library(exec_interface STATIC $<TARGET_OBJECTS:exec_interface_objects>)

target_link_libraries(
  exec_interface
  hogd
  runtime
  precompiles
  view
)

add_library(exec_interface_test_objects OBJECT
  rpc_open_proposal_block_test.cc
  rpc_loadgen_payment_test.cc
  rpc_loadgen_event_test.cc
  rpc_negative_test.cc
  rpc_from_view_gateway_test.cc
)
target_link_libraries(
  exec_interface_test_objects
  hogd_interface
  GTest::gtest
)
add_dependencies(
  exec_interface_test_objects
  all_wasm_headers
)

add_executable(
  exec_interface_test
  $<TARGET_OBJECTS:exec_interface_test_objects>
)
target_link_libraries(
  exec_interface_test
  exec_interface
  runtime_test_lib
  loadgen
  GTest::gtest
)
gtest_discover_tests(exec_interface_test)
