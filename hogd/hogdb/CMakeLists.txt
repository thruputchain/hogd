add_subdirectory(softstate)
add_subdirectory(interface)
add_subdirectory(db)

add_library(
  hogdb_core
  hogdb_core.cc
)

target_link_libraries(
  hogdb_core
  hogd
  protocol
  hogdb_softstate
  hogdb_interface
  hogdb_db
)

# Build the backend database (hogdb) executable
add_executable(
  hogdb
  hogdb.cc
)

target_link_libraries(
  hogdb
  hogdb_core
)
