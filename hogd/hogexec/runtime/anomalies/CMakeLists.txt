add_library(anomalies_test_objects OBJECT
  rcq_cyclic_cross_queue_observation_test.cc
  rcq_observation_by_value_not_index_test.cc
  rcq_observation_on_fault_test.cc
  replay_cache_height_budget_leak_test.cc
  try_invoke_admission_trusted_test.cc
)

target_link_libraries(
  anomalies_test_objects
  GTest::gtest
  hogd_interface
)

target_include_directories(
  anomalies_test_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

add_executable(
  anomalies_test
  anomalies_test_main.cc
  $<TARGET_OBJECTS:anomalies_test_objects>
)

target_link_libraries(
  anomalies_test
  precompiles
  runtime_test_lib
  storage_cost
  model_checker
  GTest::gtest
)
gtest_discover_tests(anomalies_test)
