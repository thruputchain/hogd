add_library(
  interface
  exec_coord_server.cc
  executor_state_manager.cc
  rpc_interface.cc
)

target_link_libraries(
  interface
  protocol
  hogd
  runtime
)