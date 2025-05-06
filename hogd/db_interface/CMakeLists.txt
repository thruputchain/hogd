add_library(
  db_interface
  block_manager.cc
  block_state.cc
  storage_view.cc
  transaction_state.cc
)

target_link_libraries(
  db_interface
  hogd
  storage_item
  mock_db
)

add_dependencies(
  db_interface
  protocol
)

add_executable(
  db_interface_test
  block_manager_test.cc
  transaction_state_test.cc
)

target_link_libraries(
  db_interface_test
  db_interface
  GTest::gtest_main
)
gtest_discover_tests(db_interface_test)
