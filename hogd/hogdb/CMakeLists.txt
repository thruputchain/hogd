# Create a hogdb module library
add_library(
  hogdb
  block_tx.cc
  invoke_tx.cc
  item_change.cc
  rpc_context.cc
  rpc_from_coord.cc
  rpc_from_exec.cc
  status.cc
)
target_link_libraries(
  hogdb
  ramdb
  hogd
  merkle
  genesis
)

# Build the backend database (hogdb) executable
add_executable(
  hogdb_exec
  hogdb.cc
)
set_property(TARGET hogdb_exec PROPERTY OUTPUT_NAME hogdb)
target_link_libraries(
  hogdb_exec
  hogdb
)

# Build the hogdb_bench executable
add_executable(
  hogdb_bench
  hogdb_bench.cc
)
target_link_libraries(
  hogdb_bench
  hogdb
)
