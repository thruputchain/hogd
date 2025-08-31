add_library(loadgen
  erc20_experiment.cc
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
)
gtest_discover_tests(loadgen_tests)

target_link_libraries(
  loadgen_tests
  loadgen
  runtime_test_lib
  precompiles
  gtest
)
