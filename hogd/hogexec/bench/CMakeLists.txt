add_executable(
  fibonacci_bench
  fibonacci_bench.cc
)
target_link_libraries(
  fibonacci_bench
  hogd
  protocol
  runtime
  exec_interface
  db_interface
  contract_loader
  benchmark::benchmark
)

target_include_directories(
  fibonacci_bench
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

set_property(TARGET fibonacci_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

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
  contract_loader
)

target_include_directories(
  fibonacci_wasmonly
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

set_property(TARGET fibonacci_wasmonly PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

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
