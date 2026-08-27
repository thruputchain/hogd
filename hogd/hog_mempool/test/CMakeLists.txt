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
  all_riscv_contracts
  all_riscvs_built
)

add_library(mempool_test_objects OBJECT
  mempool_test.cc
  scheduler_test.cc
  config_hash_test.cc
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
  all_riscv_contracts
)

add_executable(
  mempool_test
  $<TARGET_OBJECTS:mempool_test_objects>
  $<TARGET_OBJECTS:mempool_test_infra>
)

target_link_libraries(mempool_test
  hog_auth
  hogd
  hog_mempool
  exec_interface
  runtime
  runtime_test_util
  contract_loader
  GTest::gtest
)

gtest_discover_tests(mempool_test)

# Startup/restart ordering coverage lives in its own binary: a mempool that
# aborts on a pre-ready auth callback would otherwise take the rest of
# mempool_test down with it.
add_executable(
  mempool_startup_order_test
  startup_order_test.cc
  $<TARGET_OBJECTS:mempool_test_infra>
)

target_include_directories(mempool_startup_order_test
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

target_link_libraries(mempool_startup_order_test
  hog_auth
  hogd
  hog_mempool
  exec_interface
  runtime
  runtime_test_util
  contract_loader
  GTest::gtest
)

add_dependencies(mempool_startup_order_test
  all_riscv_contracts
)

gtest_discover_tests(mempool_startup_order_test)

add_executable(
  coordinator
  coordinator.cc
)
target_link_libraries(
  coordinator
  hogd
  hog_mempool
)

set_property(TARGET coordinator PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

if(BUILD_BENCHMARKS)
  add_executable(
    mempool_scheduler_bench
    scheduler_bench.cc
    $<TARGET_OBJECTS:mempool_test_infra>
  )
  target_include_directories(mempool_scheduler_bench
    PRIVATE
    ${CMAKE_BINARY_DIR}
  )
  target_link_libraries(
    mempool_scheduler_bench
    benchmark::benchmark
    hog_auth
    hogd
    hog_mempool
    exec_interface
    runtime
    runtime_test_util
    contract_loader
    GTest::gtest
  )
  add_dependencies(
    mempool_scheduler_bench
    all_riscv_contracts
  )
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET mempool_scheduler_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endif()
