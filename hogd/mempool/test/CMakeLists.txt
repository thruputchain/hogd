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

# Scheduler stress tests - separated for longer-running tests.
# Run with: ./build/hogd/mempool/test/scheduler_stress_test
# For large scale tests: --gtest_also_run_disabled_tests
add_executable(
  scheduler_stress_test
  scheduler_stress_test.cc
  $<TARGET_OBJECTS:mempool_test_infra>
)

target_link_libraries(scheduler_stress_test
  hogd
  mempool
  exec_interface
  runtime
  runtime_test_util
  contract_loader
  GTest::gtest
)

# Note: Not using gtest_discover_tests to avoid slowing down regular ctest runs.
# These tests can be run manually or via a separate CI job.

if(BUILD_BENCHMARKS)
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
endif()

add_executable(
  coordinator
  coordinator.cc
)
target_link_libraries(
  coordinator
  hogd
  mempool
)

set_property(TARGET coordinator PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
