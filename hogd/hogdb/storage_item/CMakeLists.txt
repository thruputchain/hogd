add_library(
  hogdb_storage_item
  retrocausal_queue.cc
  storage_item_wrapper.cc
)

target_link_libraries(
  hogdb_storage_item
  protocol
)

add_dependencies(
  hogdb_storage_item
  protocol
)

add_executable(
  hogdb_storage_item_test
  retrocausal_queue_test.cc
)

target_link_libraries(
    hogdb_storage_item_test
  hogdb_storage_item
  GTest::gtest_main
)

gtest_discover_tests(hogdb_storage_item_test)