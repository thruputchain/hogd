add_library(exec_interface_objects OBJECT
  executor.cc
  executor_context.cc
  executor_state_manager.cc
  rpc_from_coord.cc
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
)

add_executable(
  exec_interface_test
  executor_state_manager_test.cc
  rpc_open_proposal_block_test.cc
)

target_link_libraries(
  exec_interface_test
  exec_interface
  runtime_test_lib
  GTest::gtest
)
