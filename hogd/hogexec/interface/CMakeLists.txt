add_library(exec_interface_objects OBJECT
  exec_coord_server.cc
  executor_context.cc
  executor_state_manager.cc
  rpc_interface.cc
)
target_link_libraries(
  exec_interface_objects
  hogd_interface
)

add_library(exec_interface STATIC $<TARGET_OBJECTS:exec_interface_objects>)

target_link_libraries(
  exec_interface_objects
  hogd
  runtime
)