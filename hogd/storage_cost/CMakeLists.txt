add_library(
  storage_cost
  storage_cost_params.cc
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

#add_executable(
#  storage_cost_test
#)

#target_link_libraries(
#  storage_cost_test
#  storage_cost
#  GTest::gtest_main
#)
#gtest_discover_tests(storage_cost_test)
