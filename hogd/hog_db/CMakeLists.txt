# Create a hog_db module library

add_library(hog_db_objects OBJECT
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
  hog_db_dao.cc
  hog_age_reconciliation.cc
  rpc_context.cc
  rpc_from_auth.cc
  rpc_from_exec.cc
  rpc_from_gateway.cc
  rpc_from_sched.cc
  status.cc
)
target_link_libraries(
  hog_db_objects
  hogd_interface
)

add_library(
  hog_db
  $<TARGET_OBJECTS:hog_db_objects>
)

target_link_libraries(
  hog_db
  ramdb
  hogd
  metrics
  genesis
  storage_cost
)

# Build the backend database (hog_db) executable
add_executable(
  hog-db
  hog_db.cc
)
target_link_libraries(
  hog-db
  hog_db
  metrics
  http
)
add_dependencies(hog-db all_wasm_contracts)

if(BUILD_BENCHMARKS)
  # Build the hog_db_bench executable
  add_executable(
    hog_db_bench
    hog_db_bench.cc
  )
  target_link_libraries(
    hog_db_bench
    hog_db
  )
endif()

# Build hog_db_test executable
add_executable(
  hog_db_test
  auth_subscription_test.cc
  hog_db_test.cc
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
  rpc_context_lifetime_test.cc
  rpc_from_exec_test.cc
  rpc_from_gateway_test.cc
)
target_link_libraries(
  hog_db_test
  hog_db
  model_checker
  GTest::gtest_main
)
add_dependencies(hog_db_test all_wasm_contracts)
gtest_discover_tests(
  hog_db_test
)
