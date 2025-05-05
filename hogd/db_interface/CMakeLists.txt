add_library(
  db_interface
  storage_view.cc
)

target_link_libraries(
  db_interface
  hogd
  storage_item
)

add_dependencies(
  db_interface
  protocol
)
