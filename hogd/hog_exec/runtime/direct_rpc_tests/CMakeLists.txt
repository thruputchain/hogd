add_library(direct_rpc_test_objects OBJECT
  direct_auth_data_tests.cc
  direct_balance_tests.cc
  direct_bytestring_test.cc
  direct_deploy_test.cc
  direct_nnint_test.cc
  direct_nonce_tests.cc
  direct_rcq_tests.cc
  direct_staleness_test.cc
  direct_storage_ordering_tests.cc
  reference_leak_test.cc
  shutdown_test.cc
)

target_link_libraries(
  direct_rpc_test_objects
  GTest::gtest
  hogd_interface
)

target_include_directories(
  direct_rpc_test_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

add_executable(
  direct_rpc_test
  direct_rpc_test_main.cc
  $<TARGET_OBJECTS:direct_rpc_test_objects>
)

target_link_libraries(
  direct_rpc_test
  runtime_test_lib
  storage_cost
  model_checker
  GTest::gtest
)
gtest_discover_tests(direct_rpc_test)
