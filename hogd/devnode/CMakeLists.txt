add_library(
  hog_devnode
  block_driver.cc
  bootstrap_accounts.cc
  bootstrap_manifest.cc
  hog_devnode_node.cc
  hog_devnode_tmp_dir.cc
  hosted_bootstrap_generator.cc
  manifest_parse.cc
  runtime_manifest.cc
)
target_link_libraries(
  hog_devnode
  hog_auth
  base
  contract_loader
  crypto
  hog_event
  exec_interface
  genesis
  hogd
  hog_db
  hog_indexer
  hog_mempool
  protocol
  ramdb
  runtime
  storage_cost
)
add_dependencies(
  hog_devnode
  all_riscv_contracts
  all_contract_headers
)
target_include_directories(
  hog_devnode
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

add_executable(
  hog-devnode
  hog_devnode.cc
)
target_link_libraries(
  hog-devnode
  hog_devnode
)

add_executable(
  hog-hosted-bootstrap
  hosted_bootstrap.cc
)
target_link_libraries(
  hog-hosted-bootstrap
  hog_devnode
)

add_executable(
  hog_devnode_test
  hog_devnode_test.cc
)
target_compile_definitions(
  hog_devnode_test
  PRIVATE
  HOG_DEVNODE_BINARY="$<TARGET_FILE:hog-devnode>"
)
target_include_directories(
  hog_devnode_test
  PRIVATE
  ${CMAKE_BINARY_DIR}
)
target_link_libraries(
  hog_devnode_test
  contract_loader
  crypto
  genesis
  hog_devnode
  storage_cost
  GTest::gtest
)
add_dependencies(
  hog_devnode_test
  all_riscv_contracts
  hog-devnode
)
# Every test that boots a devnode holds a RESOURCE_LOCK, so the ~90 of them
# run as serial chains: 75-110s of work in CI. #1697 added the lock because
# all of them starting at once missed the readiness timeout under ASan.
# CTest starts the highest-COST tests first, and a fresh build tree has no cost
# history, so without a COST the chain starts last and runs alone on an idle
# runner after every other test has finished. A high COST starts it first, so
# it overlaps the parallel tests instead.
#
# A devnode runs a multithreaded node (a 4-thread pool, see
# kTestDevnodeThreadpoolSize), but CTest counts every test as one slot, so
# alongside a full set of parallel tests each devnode ran ~2.5x slower than
# alone and the chain still ended up last. PROCESSORS reserves the slots it
# actually uses.
#
# Even overlapped, one chain outlasted every other test in CI, so the tests
# are split by name between two locks, which lets at most two devnodes run at
# once: HogDevnode.DataDirR* (Rejects, Recovers, Replays, Resumes) is about
# half of the chain's run time, and everything else takes the other half.
# Each devnode has its own temp dir and ephemeral ports, so two don't collide.
set(HOG_DEVNODE_TEST_COST 1000)
set(HOG_DEVNODE_TEST_PROCESSORS 4)
set(HOG_DEVNODE_SECOND_LOCK_TESTS "HogDevnode.DataDirR*")
hogd_discover_tests(
  hog_devnode_test
  TEST_FILTER "${HOG_DEVNODE_SECOND_LOCK_TESTS}"
  PROPERTIES
    RESOURCE_LOCK hog_devnode_2
    COST ${HOG_DEVNODE_TEST_COST}
    PROCESSORS ${HOG_DEVNODE_TEST_PROCESSORS}
)
hogd_discover_tests(
  hog_devnode_test
  TEST_FILTER "-${HOG_DEVNODE_SECOND_LOCK_TESTS}"
  PROPERTIES
    RESOURCE_LOCK hog_devnode
    COST ${HOG_DEVNODE_TEST_COST}
    PROCESSORS ${HOG_DEVNODE_TEST_PROCESSORS}
)

add_executable(
  bootstrap_manifest_test
  bootstrap_manifest_test.cc
)
target_include_directories(
  bootstrap_manifest_test
  PRIVATE
  ${CMAKE_BINARY_DIR}
)
target_link_libraries(
  bootstrap_manifest_test
  contract_loader
  crypto
  hog_devnode
  GTest::gtest
)
add_dependencies(
  bootstrap_manifest_test
  all_riscv_contracts
  all_contract_headers
)
hogd_discover_tests(bootstrap_manifest_test)

add_executable(
  hog_devnode_tmp_dir_test
  hog_devnode_tmp_dir_test.cc
)
target_include_directories(
  hog_devnode_tmp_dir_test
  PRIVATE
  ${CMAKE_BINARY_DIR}
)
target_link_libraries(
  hog_devnode_tmp_dir_test
  hog_devnode
  GTest::gtest
)
add_dependencies(
  hog_devnode_tmp_dir_test
  all_riscv_contracts
  all_contract_headers
)
hogd_discover_tests(hog_devnode_tmp_dir_test)

add_executable(
  hosted_bootstrap_generator_test
  hosted_bootstrap_generator_test.cc
)
target_include_directories(
  hosted_bootstrap_generator_test
  PRIVATE
  ${CMAKE_BINARY_DIR}
)
target_compile_definitions(
  hosted_bootstrap_generator_test
  PRIVATE
  HOG_HOSTED_BOOTSTRAP_BINARY="$<TARGET_FILE:hog-hosted-bootstrap>"
)
target_link_libraries(
  hosted_bootstrap_generator_test
  contract_loader
  crypto
  hog_devnode
  GTest::gtest
)
add_dependencies(
  hosted_bootstrap_generator_test
  all_riscv_contracts
  all_contract_headers
  hog-hosted-bootstrap
)
hogd_discover_tests(hosted_bootstrap_generator_test)

add_executable(
  hog_devnode_fresh_launch_test
  hog_devnode_fresh_launch_test.cc
)
target_compile_definitions(
  hog_devnode_fresh_launch_test
  PRIVATE
  HOG_DEVNODE_BINARY="$<TARGET_FILE:hog-devnode>"
)
add_dependencies(
  hog_devnode_fresh_launch_test
  hog-devnode
)
add_test(
  NAME HogDevnode.FreshNonInteractiveLaunchEmitsReadiness
  COMMAND hog_devnode_fresh_launch_test
)
set_tests_properties(
  HogDevnode.FreshNonInteractiveLaunchEmitsReadiness
  PROPERTIES
    RESOURCE_LOCK hog_devnode
    COST ${HOG_DEVNODE_TEST_COST}
    PROCESSORS ${HOG_DEVNODE_TEST_PROCESSORS}
)
