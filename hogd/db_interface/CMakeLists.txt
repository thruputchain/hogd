add_library(
  db_interface
  storage_view.cc
  transaction_state.cc
)

target_link_libraries(
  db_interface
  hogd
  storage_item
)

add_dependencies(
  db_interface
  protocol
)

add_executable(
  db_interface_test
  transaction_state_test.cc
)

target_link_libraries(
  db_interface_test
  db_interface
  GTest::gtest_main
)
gtest_discover_tests(db_interface_test)
