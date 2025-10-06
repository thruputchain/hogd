add_executable(
  fibonacci_wasmonly
  fibonacci_wasmonly.cc
)
target_link_libraries(
  fibonacci_wasmonly
  hogd
  protocol
  runtime
  exec_interface
  db_interface
  hogdb
  runtime_test_util #TODO(geoff): for the local event server; remove this once we have a proper event server
  contract_loader
)

target_include_directories(
  fibonacci_wasmonly
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

set_property(TARGET fibonacci_wasmonly PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

add_executable(
  payments_bench
  payments_bench.cc
)
target_link_libraries(
  payments_bench
  hogd
  protocol
  runtime
  exec_interface
  db_interface
  hogdb
  runtime_test_util #TODO(geoff): for the local event server; remove this once we have a proper event server
  contract_loader
  loadgen
)

target_include_directories(
  payments_bench
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

set_property(TARGET payments_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

add_executable(
  bench_server
  bench_server.cc
)
target_link_libraries(
  bench_server
  hogd
  protocol
  runtime
  hogdb
)

set_property(TARGET bench_server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

add_executable(
  control_server
  control_server.cc
)
target_link_libraries(
  control_server
  hogd
  protocol
  runtime
  hogdb
)

set_property(TARGET control_server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

add_executable(
  control_cli
  control_cli.cc
)
target_link_libraries(
  control_cli
  hogd
  protocol
)

set_property(TARGET control_cli PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

