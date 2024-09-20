# create base module library
add_library(
  base
  init.cc
  log.cc
  status.cc
)

# create base module test binary
add_executable(
  base_test
  bits_test.cc
  cpputil_test.cc
  status_test.cc
  string_test.cc
)
target_link_libraries(
  base_test
  base
  GTest::gtest_main
)
gtest_discover_tests(base_test)
