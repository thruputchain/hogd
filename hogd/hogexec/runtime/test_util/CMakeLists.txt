add_library(
  runtime_test_util_objects OBJECT
  block_snapshot_server.cc
  queue_util.cc
)
target_link_libraries(
  runtime_test_util_objects
  hogd_interface
)

add_library(runtime_test_util STATIC $<TARGET_OBJECTS:runtime_test_util_objects>)

target_link_libraries(
  runtime_test_util
  hogd
  storage_item
)
