add_library(event_objects OBJECT
  event_block_context.cc
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
  hogd
  runtime_test_util
  storage_item
)

set_property(TARGET event_exec PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
