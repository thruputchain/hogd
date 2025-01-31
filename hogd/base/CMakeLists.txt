# create base module library
add_library(
  base
  init.cc
  log.cc
  status.cc
  string.cc
)

target_link_libraries(
  base
  protocol
  crypto
  ${LIBSODIUM_LIBRARIES}
  crc32c)

add_dependencies(base crypto crc32c)

# create base module test binary
add_executable(
  base_test
  bits_test.cc
  cpputil_test.cc
  hashmap_test.cc
  perthread_test.cc
  status_test.cc
  string_test.cc
  unique_function_test.cc
  worker_pool_test.cc
)
target_link_libraries(
  base_test
  base
  ${LIBSODIUM_LIBRARIES}
  GTest::gtest_main
)
gtest_discover_tests(base_test)


add_executable(
  hashmap_bench
  hashmap_bench.cc
)
target_link_libraries(
  hashmap_bench
  base
  benchmark::benchmark
)
