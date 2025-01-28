add_library(
  coord_core
  block_frame.cc
  executor_manager.cc
  allocate_service.cc
)

target_link_libraries(
  coord_core
  protocol
  hogd
)

add_executable(
  coord
  coord.cc
)

target_link_libraries(
  coord
  protocol
  hogd
  coord_core
)

add_executable(
  cli
  coord_cli.cc
)
target_link_libraries(
  cli
  protocol
  hogd
  coord_core
)

