# create metrics module library
add_library(
  metrics
  metrics.cc
)
target_link_libraries(
  metrics
  base
)

# create metrics module test binary
add_executable(
  metrics_test
  metrics_test.cc
)
target_link_libraries(
  metrics_test
  metrics
  GTest::gtest_main
)
gtest_discover_tests(metrics_test)
