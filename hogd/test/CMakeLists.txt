add_library(
  hogd_test_lib
  local_test_harness.cc
)

target_link_libraries(
  hogd_test_lib
  protocol
)

add_dependencies(
  hogd_test_lib
  rs_test_contracts
)


add_executable(
  hogd_test
  test_bytestring_write.cc
)

target_link_libraries(
  hogd_test
  hogd_test_lib
  hogexec_core
  hogdb_core
  GTest::gtest_main
)
gtest_discover_tests(hogd_test)
