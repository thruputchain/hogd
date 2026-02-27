add_library(components_objects OBJECT
  hash_buffers.cc
)
target_link_libraries(components_objects hogd_interface)
target_include_directories(components_objects PRIVATE ${CMAKE_BINARY_DIR})
add_library(components STATIC $<TARGET_OBJECTS:components_objects>)
target_link_libraries(components crypto)

# Own test executable (same pattern as runtime_test)
add_library(components_test_objects OBJECT
  hash_buffers_test.cc
  alias_guard_test.cc
)
target_link_libraries(components_test_objects GTest::gtest hogd_interface)
target_include_directories(components_test_objects PRIVATE ${CMAKE_BINARY_DIR})

add_executable(components_test $<TARGET_OBJECTS:components_test_objects>)
target_link_libraries(components_test
  components
  runtime
  hogd
  GTest::gtest
  GTest::gtest_main
)
gtest_discover_tests(components_test)
