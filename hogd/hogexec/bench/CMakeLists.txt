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
  benchmark::benchmark
)

set_property(TARGET fibonacci_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
