add_library(db_interface_objects OBJECT
  block_manager.cc
  block_state.cc
  db_sched_server.cc
  db_executor_server.cc
  storage_view.cc
  transaction_state.cc
)
target_link_libraries(
  db_interface_objects
  hogd_interface
)

add_library(db_interface STATIC
  $<TARGET_OBJECTS:db_interface_objects>
)

target_link_libraries(
  db_interface
  hogd
  storage_item
  storage_cost
  mock_db
  genesis
)

add_library(db_interface_test_objects OBJECT
  block_manager_test.cc
  transaction_state_test.cc
)
target_link_libraries(
  db_interface_test_objects
  hogd_interface
  GTest::gtest
)

add_executable(
  db_interface_test
  $<TARGET_OBJECTS:db_interface_test_objects>
)

target_link_libraries(
  db_interface_test
  db_interface
  GTest::gtest_main
)
gtest_discover_tests(db_interface_test)
