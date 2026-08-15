add_library(
  loadgen
  erc20_experiment.cc
  erc20_basic_auth_experiment.cc
  loadgen_server.cc
  status.cc
)

target_link_libraries(loadgen
  hogd
  genesis
)

add_dependencies(
  loadgen
  all_wasm_contracts
  all_wasm_headers
  all_riscvs_built # both experiments load .rv32a artifacts at runtime
)

target_include_directories(loadgen PRIVATE
  ${CMAKE_BINARY_DIR}
  ${CMAKE_BINARY_DIR}/hogd/xdr
)

add_executable(
  loadgen_tests
  erc20_experiment_tests.cc
  loadgen_tests.cc
)
gtest_discover_tests(loadgen_tests)

target_link_libraries(
  loadgen_tests
  loadgen
  runtime_test_lib
  precompiles
  gtest
)

add_dependencies(
  loadgen_tests
  all_wasm_contracts
  all_wasm_headers
  all_riscvs_built # for itc_erc20.rv32a / simple_wallet.rv32a (BasicRv32 test)
)

add_executable(
  loadgen_server
  main.cc
)

target_link_libraries(
  loadgen_server
  loadgen
  metrics
  http
  hogd
)

if(BUILD_BENCHMARKS)
  # create loadgen benchmark binary
  add_executable(
    erc20_experiment_bench
    erc20_experiment_bench.cc
  )
  target_link_libraries(
    erc20_experiment_bench
    loadgen
    benchmark::benchmark
  )

  add_dependencies(
    erc20_experiment_bench
    all_wasm_contracts
    all_wasm_headers
  )
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET erc20_experiment_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endif()

# create tx_size_stats binary
add_executable(
  tx_size_stats
  tx_size_stats.cc
)
target_link_libraries(
  tx_size_stats
  loadgen
  hogd
)

add_dependencies(
  tx_size_stats
  all_wasm_contracts
  all_wasm_headers
)

# create gas_usage_metric binary
add_executable(
  gas_usage_metric
  gas_usage_metric.cc
)
target_link_libraries(
  gas_usage_metric
  loadgen
  hogd
  protocol
  runtime_test_util
  runtime_test_lib
  precompiles
  gtest
)

add_dependencies(
  gas_usage_metric
  all_wasm_contracts
  all_wasm_headers
)

# in-process payments execution benchmark (wasm vs rv32), no servers needed
if(BUILD_BENCHMARKS)
  add_executable(
    payments_exec_bench
    payments_exec_bench.cc
  )
  target_link_libraries(
    payments_exec_bench
    loadgen
    hogd
    protocol
    runtime_test_util
    runtime_test_lib
    precompiles
    gtest
    benchmark::benchmark
  )

  add_dependencies(
    payments_exec_bench
    all_wasm_contracts
    all_wasm_headers
    all_riscvs_built # for itc_erc20.rv32a / simple_wallet.rv32a
  )
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET payments_exec_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endif()
