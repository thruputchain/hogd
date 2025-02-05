add_library(
    loadgen
    payment_experiment.cc
)

target_link_libraries(
    loadgen
    hogd
    protocol
)

add_executable(
    test_loadgen
    test_payment_experiment.cc
)

target_link_libraries(
    test_loadgen
    loadgen
    hogd
    hogexec_core
    hogdb_core
    coord_core
    GTest::gtest_main
)
gtest_discover_tests(test_loadgen)

add_executable(
  payments_bench
  payments_bench.cc
)
target_link_libraries(
  payments_bench
  loadgen
  hogd
  hogexec_core
  hogdb_core
  coord_core
  benchmark::benchmark
)

set_property(TARGET payments_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
