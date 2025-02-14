# Build the backend database (hogdb) executable
add_executable(
  hogdb
  hogdb.cc
)
target_link_libraries(
  hogdb
  hogd
  ramdb
)
