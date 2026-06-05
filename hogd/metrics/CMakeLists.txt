# create metrics module library
add_library(
  metrics
  metrics.cc
)
target_link_libraries(
  metrics
  base
)

add_library(
  new_metrics
  health.cc
  new_metrics.cc
)
target_link_libraries(
  new_metrics
  base
  hogd
  http
  x23
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

add_executable(
  health_test
  health_test.cc
)
target_link_libraries(
  health_test
  new_metrics
  GTest::gtest_main
)
gtest_discover_tests(health_test)
