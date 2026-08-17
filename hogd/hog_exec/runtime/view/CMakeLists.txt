add_library(view_objects OBJECT
  view.cc
)
target_link_libraries(
  view_objects
  hogd_interface
)

add_library(view STATIC $<TARGET_OBJECTS:view_objects>)
target_link_libraries(
  view
  runtime
  precompiles
)

add_library(view_test_objects OBJECT
  view_test.cc
)
target_link_libraries(
  view_test_objects
  hogd_interface
  GTest::gtest
)
add_dependencies(
  view_test_objects
  all_wasm_headers
)

add_executable(
  view_test
  view_test_main.cc
  $<TARGET_OBJECTS:view_test_objects>
)
target_link_libraries(
  view_test
  view
  runtime_test_lib
  GTest::gtest
)
add_dependencies(
  view_test
  all_riscvs_built
)

gtest_discover_tests(view_test)
