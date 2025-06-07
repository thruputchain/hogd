add_library(
  exec_interface
  exec_coord_server.cc
  executor_context.cc
  executor_state_manager.cc
  rpc_interface.cc
)

target_link_libraries(
  exec_interface
  protocol
  hogd
  runtime
)