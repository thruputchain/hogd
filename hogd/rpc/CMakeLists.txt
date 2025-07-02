# create rpc module library
add_library(
  rpc
  rpc.cc
)
target_link_libraries(
  rpc
  hogd
)

# Build the rpc_bench executable
add_executable(
  rpc_bench
  rpc_bench.cc
)
target_link_libraries(
  rpc_bench
  rpc
)

# create test binary
add_executable(
  rpc_test
  rpc_test.cc
)
target_link_libraries(
  rpc_test
  rpc
  GTest::gtest_main
)
gtest_discover_tests(
  rpc_test
)
