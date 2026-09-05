add_library(
  runtime_test_util_objects OBJECT
  block_snapshot_server.cc
  hog_db_wrapper.cc
  queue_util.cc
  test_event_aggregator.cc
  test_sched_db_server.cc
)
target_link_libraries(
  runtime_test_util_objects
  hogd_interface
)

add_library(runtime_test_util STATIC $<TARGET_OBJECTS:runtime_test_util_objects>)

target_link_libraries(
  runtime_test_util
  hogd
  hog_db
  storage_item
)

