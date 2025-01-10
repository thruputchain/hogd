add_library(
  hogdb_softstate
  storage_item_frame.cc
)

target_link_libraries(
  hogdb_softstate
  protocol
)

add_dependencies(
  hogdb_softstate
  protocol
)
