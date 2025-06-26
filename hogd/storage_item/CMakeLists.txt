add_library(
  storage_item_objects OBJECT
  refcounted_delta_variant.cc
  retrocausal_queue.cc
  replay_cache.cc
  storage_item_wrapper.cc
  storage_lock_util.cc
  storage_item_delta_manager.cc
)

add_library(storage_item STATIC $<TARGET_OBJECTS:storage_item_objects>)

target_link_libraries(
  storage_item_objects
  hogd_interface
)

target_link_libraries(
  storage_item
  storage_cost
  hogd
)

add_library(storage_item_test_objects OBJECT
  retrocausal_queue_test.cc
  storage_item_delta_manager_test.cc
  refcounted_delta_variant_test.cc
  replay_cache_test.cc
)

target_link_libraries(
  storage_item_test_objects
  hogd_interface
  GTest::gtest
)

add_executable(
  storage_item_test
  $<TARGET_OBJECTS:storage_item_test_objects>
)

target_link_libraries(
  storage_item_test
  storage_item
  model_checker
  GTest::gtest_main
)
gtest_discover_tests(storage_item_test)
