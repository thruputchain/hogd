# Build the backend database (hogdb) executable
add_executable(
  hogdb
  block_tx.cc
  hogdb.cc
  status.cc
)
target_link_libraries(
  hogdb
  hogd
  ramdb
)
