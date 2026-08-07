add_library(item_objects OBJECT
  item_view.cc
  snapshot_storage_item.cc
  database_proxy.cc
  storage_snapshot_cache.cc
)
target_link_libraries(item_objects hogd_interface)
target_include_directories(item_objects PRIVATE ${CMAKE_BINARY_DIR})

# Standalone item tests — no syscall handlers, WASM runtimes, or precompiles
add_library(item_test_objects OBJECT
  item_view_test.cc
  snapshot_storage_item_test.cc
)
target_link_libraries(item_test_objects GTest::gtest hogd_interface)
target_include_directories(item_test_objects PRIVATE ${CMAKE_BINARY_DIR})

add_executable(item_test
  item_test_main.cc
  $<TARGET_OBJECTS:item_test_objects>
)
target_link_libraries(item_test
  runtime
  runtime_test_util
  hog_exec_common
  components
  protocol
  storage_cost
  GTest::gtest
)
gtest_discover_tests(item_test)
