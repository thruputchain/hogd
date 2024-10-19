add_library(
  softstate
  delta_class_checker.cc
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
)

target_link_libraries(
  softstate_test
  softstate
  GTest::gtest_main
)

gtest_discover_tests(softstate_test)

