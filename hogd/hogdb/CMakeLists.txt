add_subdirectory(softstate)
add_subdirectory(interface)
add_subdirectory(db)

# Build the backend database (hogdb) executable
add_executable(
  hogdb
  hogdb.cc
  hogdb_core.cc
)
target_link_libraries(
  hogdb
  hogd
  hogdb_softstate
  hogdb_interface
  hogdb_db
)
