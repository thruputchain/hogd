add_library(
  storage_item
  refcounted_delta_variant.cc
  retrocausal_queue.cc
  storage_item_wrapper.cc
  storage_lock_util.cc
  storage_item_delta_manager.cc
)

target_link_libraries(
  storage_item
  hogd
)

add_dependencies(
  storage_item
  hogd
)

add_executable(
  storage_item_test
  retrocausal_queue_test.cc
  storage_item_delta_manager_test.cc
  refcounted_delta_variant_test.cc
  wide_integral_test.cc
)

target_link_libraries(
  storage_item_test
  storage_item
  GTest::gtest_main
)
gtest_discover_tests(storage_item_test)
