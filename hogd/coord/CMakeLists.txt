add_executable(
  coord
  coord.cc
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

