add_executable(
  get_port
  get_port.cc
)

target_link_libraries(
  get_port
  hogd
)