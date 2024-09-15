add_library(
  storage_proxy
  storage_object_proxy.cc
)

add_executable(
  storage_proxy_test
  copy_on_write_test.cc

)

target_link_libraries(
  storage_proxy_test
  storage_proxy
  GTest::gtest_main
)
gtest_discover_tests(storage_proxy_test)
