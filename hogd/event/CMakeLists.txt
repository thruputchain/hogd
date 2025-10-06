add_library(event_objects OBJECT
  event.cc
)

target_link_libraries(
  event_objects
  hogd_interface
)

add_library(event STATIC $<TARGET_OBJECTS:event_objects>)

target_link_libraries(
  event
  hogd
)

add_dependencies(
  event_objects
  hogd
)

target_include_directories(
  event_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)
