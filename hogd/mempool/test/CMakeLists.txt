add_executable(
  auth_test
  auth_test.cc
)
target_link_libraries(
  auth_test
  base
  crypto
  db_interface
  exec_interface
  hogd
  mempool
  merkle
  protocol
  runtime
  runtime_test_util
  contract_loader
  GTest::gtest_main
)
# for the generated contract headers
target_include_directories(
  auth_test
  PRIVATE
  ${CMAKE_BINARY_DIR}
)
set_property(TARGET auth_test PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
gtest_discover_tests(auth_test)

add_executable(
  mempool_test
  mempool_test.cc
)
target_link_libraries(
  mempool_test
  base
  crypto
  db_interface
  exec_interface
  hogd
  mempool
  merkle
  protocol
  runtime
  runtime_test_util
  contract_loader
  GTest::gtest_main
)
# for the generated contract headers
target_include_directories(
  mempool_test
  PRIVATE
  ${CMAKE_BINARY_DIR}
)
set_property(TARGET mempool_test PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
gtest_discover_tests(mempool_test)

add_executable(
  mempool_bench
  mempool_bench.cc
)
target_link_libraries(
  mempool_bench
  base
  crypto
  db_interface
  exec_interface
  hogd
  loadgen
  mempool
  merkle
  protocol
  runtime
  runtime_test_util
  contract_loader
  benchmark::benchmark
)
# for the generated contract headers
target_include_directories(
  mempool_bench
  PRIVATE
  ${CMAKE_BINARY_DIR}
)
set_property(TARGET mempool_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

add_executable(
  scheduler_test
  scheduler_test.cc
)
target_link_libraries(
  scheduler_test
  base
  crypto
  db_interface
  exec_interface
  hogd
  mempool
  merkle
  protocol
  runtime
  runtime_test_util
  GTest::gtest_main
)
# for the generated contract headers
target_include_directories(
  scheduler_test
  PRIVATE
  ${CMAKE_BINARY_DIR}
)
set_property(TARGET scheduler_test PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
gtest_discover_tests(scheduler_test)
