add_library(
  interface
  rpc_interface.cc
  executor_state_manager.cc
)

target_link_libraries(
  interface
  protocol
  hogd
  runtime
)