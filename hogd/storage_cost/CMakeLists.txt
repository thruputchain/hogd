add_library(
  storage_cost
  storage_cost_model.cc
)

target_link_libraries(
  storage_cost
  hogd
)

add_dependencies(
  storage_cost
  hogd
)

add_executable(
  storage_cost_test
  storage_cost_model_test.cc
)

target_link_libraries(
  storage_cost_test
  storage_cost
  hogd
  GTest::gtest_main
)
hogd_discover_tests(storage_cost_test)
