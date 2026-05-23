# Create a hogdb module library

add_library(hogdb_objects OBJECT
  auth_change_subscription_manager.cc
  background_commit_worker.cc
  block_tx.cc
  change_set.cc
  block_id_manager.cc
  item_lock/auth_lock.cc
  item_lock/balance_lock.cc
  item_lock/bytestring_lock.cc
  item_lock/deletion_lock.cc
  item_lock/exclusive_lock.cc
  item_lock/new_contract_lock.cc
  item_lock/new_deployment_lock.cc
  item_lock/nnint128_lock.cc
  item_lock/replay_cache_lock.cc
  item_lock/retrocausal_queue_lock.cc
  item_lock.cc
  invariant_hog_age.cc
  invariant_hog_conservation.cc
  invoke_tx.cc
  item_change.cc
  hogdb_dao.cc
  hog_age_reconciliation.cc
  rpc_context.cc
  rpc_from_auth.cc
  rpc_from_exec.cc
  rpc_from_gateway.cc
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
  health
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
  health
)
add_dependencies(hogdb_exec all_wasm_contracts)

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
  auth_subscription_test.cc
  hogdb_test.cc
  item_change_auth_test.cc
  item_change_exclusive_test.cc
  item_lock_test.cc
  item_lock/auth_lock_test.cc
  item_lock/balance_lock_test.cc
  item_lock/bytestring_lock_test.cc
  item_lock/nnint128_lock_test.cc
  item_lock/replay_cache_lock_test.cc
  mock_authdb_rpc.cc
  recovery_tests.cc
  rpc_from_gateway_test.cc
)
target_link_libraries(
  hogdb_test
  hogdb
  model_checker
  GTest::gtest_main
)
add_dependencies(hogdb_test all_wasm_contracts)
gtest_discover_tests(
  hogdb_test
)
