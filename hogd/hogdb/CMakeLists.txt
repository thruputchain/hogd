# Create a hogdb module library

add_library(hogdb_objects OBJECT
  block_tx.cc
  block_id_manager.cc
  invariant_hog_conservation.cc
  invoke_tx.cc
  item_change.cc
  hogdb_dao.cc
  rpc_context.cc
  rpc_from_exec.cc
  rpc_from_sched.cc
  status.cc
)
target_link_libraries(
  hogdb_objects
  hogd_interface
)

add_library(
  hogdb
  $<TARGET_OBJECTS:hogdb_objects>
)

target_link_libraries(
  hogdb
  ramdb
  hogd
  merkle
  genesis
  storage_cost
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

# Build hogdb_test executable
add_executable(
  hogdb_test
  hogdb_test.cc
  recovery_tests.cc
)
target_link_libraries(
  hogdb_test
  hogdb
  GTest::gtest_main
)
gtest_discover_tests(
  hogdb_test
)

