add_library(
  mempool
  rpc_from_coord.cc
  scheduler.cc
)
target_link_libraries(
  mempool
  base
  crypto
  exec_interface
  hogd
  hogdb
  merkle
  protocol
  runtime
)
set_property(TARGET mempool PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

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
  mempool
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
  mempool
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
  mempool
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
