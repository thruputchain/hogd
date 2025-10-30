add_subdirectory(merkle)

# create container module test binary
add_executable(
  container_test
  synchronized_map_test.cc
  concurrent_map_test.cc
)
target_link_libraries(
  container_test
  base
  GTest::gtest_main
)
gtest_discover_tests(container_test)
