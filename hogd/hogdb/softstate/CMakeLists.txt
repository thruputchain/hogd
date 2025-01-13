add_library(
  hogdb_softstate
  contract_map.cc
  storage_item_frame.cc
  storage_view.cc
)

target_link_libraries(
  hogdb_softstate
  protocol
)

add_dependencies(
  hogdb_softstate
  protocol
)

add_executable(
  hogdb_softstate_test
  storage_item_frame_test.cc
)

target_link_libraries(
  hogdb_softstate_test
  hogdb_softstate
  GTest::gtest_main
)
gtest_discover_tests(hogdb_softstate_test)
