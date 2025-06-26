
add_library(
  mock_db
  in_memory_database.cc
)

target_link_libraries(
  mock_db
  hogd
  storage_item
)

add_dependencies(
  mock_db
  protocol
)
