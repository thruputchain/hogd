add_library(heap_profile heap_profile.cc)
add_executable(heap_profile_test heap_profile_test.cc)
target_link_libraries(heap_profile_test heap_profile)
add_test(NAME heap_profile_test COMMAND heap_profile_test
  ${CMAKE_CURRENT_SOURCE_DIR}/testdata/jemalloc.heap)

add_library(
  metrics
  health.cc
  metrics.cc
)
target_link_libraries(
  metrics
  base
  hogd
  http
  x23
)

add_executable(
  health_test
  health_test.cc
)
target_link_libraries(
  health_test
  metrics
  GTest::gtest_main
)
hogd_discover_tests(health_test)

add_executable(
  histogram_test
  histogram_test.cc
)
target_link_libraries(
  histogram_test
  metrics
  GTest::gtest_main
)
hogd_discover_tests(histogram_test)

add_executable(
  summary_test
  summary_test.cc
)
target_link_libraries(
  summary_test
  metrics
  GTest::gtest_main
)
hogd_discover_tests(summary_test)

add_executable(
  metrics_test
  metrics_test.cc
)
target_link_libraries(
  metrics_test
  metrics
  GTest::gtest_main
)
hogd_discover_tests(metrics_test)
