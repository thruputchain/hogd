add_library(
  hogdb_interface
  block_manager.cc
  db_to_executor_server.cc
  db_to_coord_server.cc
)

target_link_libraries(
  hogdb_interface
  protocol
  hogd
  hogdb_storage_item
  hogdb_softstate
  hogdb_db
)

add_executable(
  hogdb_interface_test
  block_manager_test.cc
)

target_link_libraries(
  hogdb_interface_test
  hogdb_interface
  GTest::gtest_main
)

gtest_discover_tests(hogdb_interface_test)