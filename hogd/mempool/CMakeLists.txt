add_library(
  mempool
  mempool.cc
)
target_link_libraries(
  mempool
  base
  crypto
  db_interface
  exec_interface
  hogd
  hogdb
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
  contract_loader
  GTest::gtest_main
)

set_property(TARGET mempool_test PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
gtest_discover_tests(mempool_test)
