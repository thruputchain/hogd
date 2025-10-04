# create base module library
add_library(
  base_objects OBJECT
  init.cc
  io.cc
  log.cc
  os.cc
  status.cc
  string.cc
)
target_link_libraries(
  base_objects
  built_headers_interface
  protocol
)
add_library(base STATIC $<TARGET_OBJECTS:base_objects>)

add_library(base_test_objects OBJECT
  base_test.cc
  bits_test.cc
  cpputil_test.cc
  io_test.cc
  perthread_test.cc
  status_test.cc
  string_test.cc
  unique_function_test.cc
  wide_integral_test.cc
  worker_pool_test.cc
)
target_link_libraries(
  base_test_objects
  GTest::gtest
)

# create base module test binary
add_executable(
  base_test
  $<TARGET_OBJECTS:base_test_objects>
)

target_link_libraries(
  base_test
  hogd
  GTest::gtest
)
gtest_discover_tests(base_test)
