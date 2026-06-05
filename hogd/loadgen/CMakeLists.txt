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

add_executable(
  loadgen_server
  main.cc
)

target_link_libraries(
  loadgen_server
  loadgen
  health
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
