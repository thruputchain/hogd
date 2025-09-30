add_library(
  libmempool
  rpc_from_coord.cc
  rpc_from_db.cc
  rpc_from_exec.cc
  scheduler.cc
)
target_link_libraries(
  libmempool
  base
  crypto
  exec_interface
  hogd
  hogdb
  merkle
  protocol
  runtime
)
set_property(TARGET libmempool PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

add_executable(
  mempool 
  mempool.cc
)
target_link_libraries(
  mempool
  base
  crypto
  exec_interface
  hogd
  libmempool
  merkle
  protocol
  runtime
  contract_loader
  GTest::gtest_main
)

add_executable(
  auth_test
  auth_test.cc
)
target_link_libraries(
  auth_test
  base
  crypto
  exec_interface
  hogd
  libmempool
  merkle
  protocol
  runtime
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
  exec_interface
  hogd
  libmempool
  merkle
  protocol
  runtime
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
  scheduler_test
  scheduler_test.cc
)
target_link_libraries(
  scheduler_test
  base
  crypto
  exec_interface
  hogd
  libmempool
  merkle
  protocol
  runtime
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
