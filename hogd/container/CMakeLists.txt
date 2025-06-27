add_subdirectory(merkle)

# create container module test binary
add_executable(
  synchronized_map_test
  synchronized_map_test.cc
)
target_link_libraries(
  synchronized_map_test
  base
  GTest::gtest_main
)
gtest_discover_tests(synchronized_map_test)
