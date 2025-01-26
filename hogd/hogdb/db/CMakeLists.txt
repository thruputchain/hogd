add_library(
  hogdb_db
  in_memory_database.cc
)

target_link_libraries(
  hogdb_db
  protocol
  hogd
  hogdb_storage_item
)
