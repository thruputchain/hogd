# Build the backend database (hogdb) executable
add_executable(
  hogdb
  hogdb.cc
  block_tx.cc
  invoke_tx.cc
  item_change.cc
  status.cc
)
target_link_libraries(
  hogdb
  hogd
  ramdb
)
