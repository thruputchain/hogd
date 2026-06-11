add_library(event_objects OBJECT
  event_block_context.cc
  rpc_context.cc
  rpc_from_exec.cc
  rpc_from_sched.cc
  status.cc
  topic_layout.cc
  utilities.cc
)

target_link_libraries(
  event_objects
  hogd_interface
)

add_library(event STATIC $<TARGET_OBJECTS:event_objects>)

target_link_libraries(
  event
  hogd
  metrics
  merkle
)

target_include_directories(
  event_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

add_executable(
  event_exec
  event.cc
)
target_link_libraries(
  event_exec
  event
  metrics
  http
  hogd
)

set_property(TARGET event_exec PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

add_library(event_test_objects OBJECT
  event_block_context_test.cc
  event_rpc_test.cc
  topic_layout_test.cc
)
target_link_libraries(
  event_test_objects
  hogd_interface
  GTest::gtest
)

add_executable(
  event_test
  $<TARGET_OBJECTS:event_test_objects>
)
target_link_libraries(
  event_test
  hogd
  event
  GTest::gtest
)
gtest_discover_tests(event_test)

add_executable(
  event_server_bench
  event_server_bench.cc
)
target_link_libraries(
  event_server_bench
  event
  hogd
  protocol
)

target_include_directories(
  event_server_bench
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

set_property(TARGET event_server_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
