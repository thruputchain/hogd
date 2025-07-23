add_library(
  mempool
  mempool.cc
  rpc_interface.cc
)
target_link_libraries(
  mempool
  base
  crypto
  db_interface
  exec_interface
  hogd
  merkle
  protocol
  runtime
)
set_property(TARGET mempool PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

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
  GTest::gtest_main
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
  mempool
  merkle
  protocol
  runtime
  benchmark::benchmark
)
set_property(TARGET mempool_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
