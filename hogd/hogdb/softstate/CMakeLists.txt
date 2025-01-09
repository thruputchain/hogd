add_library(
  hogdb_softstate
  contract_map.cc
)

target_link_libraries(
  hogdb_softstate
  protocol
)

add_dependencies(
  hogdb_softstate
  protocol
)
