add_library(
  coord_core
  block_frame.cc
  executor_manager.cc
  allocate_service.cc
  coord_block_manager.cc
  database_manager.cc
  coord_core.cc
  coro_queue.cc
)

target_link_libraries(
  coord_core
  protocol
  hogd
)

add_executable(
  coord
  coord.cc
)

target_link_libraries(
  coord
  protocol
  hogd
  coord_core
)

add_executable(
  cli
  coord_cli.cc
)
target_link_libraries(
  cli
  protocol
  hogd
  coord_core
)

add_executable(
  coro_queue_test
  coro_queue_test.cc
)
target_link_libraries(
  coro_queue_test
  hogd
  coord_core
  GTest::gtest_main
)
gtest_discover_tests(coro_queue_test)
