add_library(
  storage_item
  retrocausal_queue.cc
  storage_item_wrapper.cc
  storage_lock_util.cc
)

target_link_libraries(
  storage_item
  protocol
)

add_dependencies(
  storage_item
  protocol
)

add_executable(
  storage_item_test
  retrocausal_queue_test.cc
)

target_link_libraries(
  storage_item_test
  storage_item
  GTest::gtest_main
)
gtest_discover_tests(storage_item_test)
