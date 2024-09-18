# create base module library
add_library(
  base
  log.cc
  status.cc
  ../xdr/types.x
)

use_x23(base)
target_include_directories(base PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

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
