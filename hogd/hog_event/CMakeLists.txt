add_library(hog_event_objects OBJECT
  event_block_context.cc
  rpc_context.cc
  rpc_from_exec.cc
  rpc_from_sched.cc
  status.cc
  topic_layout.cc
  utilities.cc
)

target_link_libraries(
  hog_event_objects
  hogd_interface
)

add_library(hog_event STATIC $<TARGET_OBJECTS:hog_event_objects>)

target_link_libraries(
  hog_event
  hogd
  metrics
  merkle
)

target_include_directories(
  hog_event_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

add_executable(
  hog-event
  hog_event.cc
)
target_link_libraries(
  hog-event
  hog_event
  metrics
  http
  hogd
)

if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  set_property(TARGET hog-event PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

add_library(hog_event_test_objects OBJECT
  event_block_context_test.cc
  event_rpc_test.cc
  rpc_server_shutdown_test.cc
  topic_layout_test.cc
)
target_link_libraries(
  hog_event_test_objects
  hogd_interface
  GTest::gtest
)

add_executable(
  hog_event_test
  $<TARGET_OBJECTS:hog_event_test_objects>
)
target_link_libraries(
  hog_event_test
  hogd
  hog_event
  GTest::gtest
)
gtest_discover_tests(hog_event_test)

add_executable(
  hog_event_server_bench
  event_server_bench.cc
)
target_link_libraries(
  hog_event_server_bench
  hog_event
  hogd
  protocol
)

target_include_directories(
  hog_event_server_bench
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  set_property(TARGET hog_event_server_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
