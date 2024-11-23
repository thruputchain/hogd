add_library(
  softstate
  storage_object_frame.cc
  local_softstate_map.cc
)

target_link_libraries(
  softstate
  protocol
  hogd
)

add_executable(
  softstate_test
  test_atomic_uint128.cc
  test_storage_object_frame.cc
  test_storage_object_locking.cc
)

target_link_libraries(
  softstate_test
  softstate
  runtime
  GTest::gtest_main
)

gtest_discover_tests(softstate_test)

