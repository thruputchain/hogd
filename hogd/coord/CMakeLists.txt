add_executable(
  coord
  coord.cc
  block_frame.cc
  executor_manager.cc
  allocate_service.cc
)
target_link_libraries(
  coord
  protocol
  hogd
)

add_executable(
  cli
  coord_cli.cc
)
target_link_libraries(
  cli
  protocol
  hogd
)

