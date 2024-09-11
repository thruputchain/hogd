add_library(
  syscall
  syscall_status.cc
  syscall_handler.cc
  proxy_applicator.cc
)

#add_executable(
#  syscall_test
#  metered_result_test.cc
#)
#target_link_libraries(
#  syscall_test
#  syscall
#  GTest::gtest_main
#)
#gtest_discover_tests(syscall_test)
