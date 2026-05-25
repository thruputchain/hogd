add_library(
  hogd_dev
  bootstrap_accounts.cc
  bootstrap_manifest.cc
  hogd_dev_node.cc
  hogd_dev_tmp_dir.cc
  manifest_parse.cc
  runtime_manifest.cc
)
target_link_libraries(
  hogd_dev
  auth
  base
  contract_loader
  crypto
  event
  exec_interface
  genesis
  hogd
  hogdb
  indexer
  mempool
  protocol
  ramdb
  runtime
  storage_cost
)
add_dependencies(
  hogd_dev
  all_wasm_contracts
  all_wasm_headers
)
target_include_directories(
  hogd_dev
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

add_executable(
  hogd_dev_exec
  hogd_dev.cc
)
set_property(TARGET hogd_dev_exec PROPERTY OUTPUT_NAME hogd-dev)
target_link_libraries(
  hogd_dev_exec
  hogd_dev
)

add_custom_target(hogd-dev DEPENDS hogd_dev_exec)

add_executable(
  hogd_dev_test
  hogd_dev_test.cc
)
target_compile_definitions(
  hogd_dev_test
  PRIVATE
  HOGD_DEV_BINARY="$<TARGET_FILE:hogd_dev_exec>"
)
target_include_directories(
  hogd_dev_test
  PRIVATE
  ${CMAKE_BINARY_DIR}
)
target_link_libraries(
  hogd_dev_test
  contract_loader
  crypto
  genesis
  hogd_dev
  storage_cost
  GTest::gtest
)
add_dependencies(
  hogd_dev_test
  all_wasm_contracts
  hogd_dev_exec
)
gtest_discover_tests(
  hogd_dev_test
  PROPERTIES
    RESOURCE_LOCK hogd_dev
)

add_executable(
  hogd_dev_fresh_launch_test
  hogd_dev_fresh_launch_test.cc
)
target_compile_definitions(
  hogd_dev_fresh_launch_test
  PRIVATE
  HOGD_DEV_BINARY="$<TARGET_FILE:hogd_dev_exec>"
)
add_dependencies(
  hogd_dev_fresh_launch_test
  hogd_dev_exec
)
add_test(
  NAME HogdDev.FreshNonInteractiveLaunchEmitsReadiness
  COMMAND hogd_dev_fresh_launch_test
)
set_tests_properties(
  HogdDev.FreshNonInteractiveLaunchEmitsReadiness
  PROPERTIES
    RESOURCE_LOCK hogd_dev
)
