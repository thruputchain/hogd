add_library(mempool_test_infra OBJECT
  infra.cc
)
target_include_directories(mempool_test_infra
  PRIVATE
  ${CMAKE_BINARY_DIR}
)
target_link_libraries(mempool_test_infra
  GTest::gtest
  hogd_interface
)
add_dependencies(mempool_test_infra
  all_wasm_contracts
)

add_library(mempool_test_objects OBJECT
  auth_test.cc
  mempool_test.cc
  scheduler_test.cc
)
target_include_directories(mempool_test_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)
target_link_libraries(mempool_test_objects
  GTest::gtest
  hogd_interface
)
add_dependencies(mempool_test_objects
  all_wasm_contracts
)

add_executable(
  mempool_test
  $<TARGET_OBJECTS:mempool_test_objects>
  $<TARGET_OBJECTS:mempool_test_infra>
)

target_link_libraries(mempool_test
  hogd
  mempool
  exec_interface
  runtime
  runtime_test_util
  contract_loader
  GTest::gtest
)

gtest_discover_tests(mempool_test)

add_executable(
  mempool_bench
  mempool_bench.cc
  $<TARGET_OBJECTS:mempool_test_infra>
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
