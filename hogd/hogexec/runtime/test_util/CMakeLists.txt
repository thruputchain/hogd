add_library(
  runtime_test_util
  block_snapshot_server.cc
)

add_dependencies(
  runtime_test_util
  hogd
  protocol
)

target_link_libraries(
  runtime_test_util
  hogd
  protocol
  storage_item
)
