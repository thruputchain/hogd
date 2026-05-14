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
  auth
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
  coordinator
  coordinator.cc
)
target_link_libraries(
  coordinator
  hogd
  mempool
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
    auth
    hogd
    mempool
    exec_interface
    runtime
    runtime_test_util
    contract_loader
    GTest::gtest
  )
  add_dependencies(
    mempool_scheduler_bench
    all_wasm_contracts
  )
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET mempool_scheduler_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endif()

if(BUILD_FUZZERS)
  add_executable(
    mempool_scheduler_fuzz
    EXCLUDE_FROM_ALL
    scheduler_fuzz.cc
    $<TARGET_OBJECTS:mempool_test_infra>
  )
  target_include_directories(mempool_scheduler_fuzz
    PRIVATE
    ${CMAKE_BINARY_DIR}
  )
  target_link_libraries(
    mempool_scheduler_fuzz
    auth
    hogd
    mempool
    exec_interface
    runtime
    runtime_test_util
    contract_loader
    GTest::gtest
  )
  add_dependencies(
    mempool_scheduler_fuzz
    all_wasm_contracts
  )
  target_compile_options(
    mempool_scheduler_fuzz
    PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=all
  )
  # libclang_rt.fuzzer is built against libstdc++, but this project uses
  # libc++. Suppress automatic stdlib linking and explicitly link both.
  target_link_options(
    mempool_scheduler_fuzz
    PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=all
    -nostdlib++
    -lc++
    -lstdc++
  )
endif()
