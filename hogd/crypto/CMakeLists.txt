# create crypto module library
add_library(
  crypto
  crypto.cc
)
add_dependencies(crypto libsodium)
target_link_libraries(
  crypto
  protocol
  libsodium_lib
  crc32c
)

# create crypto module test binary
add_executable(
  crypto_test
  crypto_test.cc
)
target_link_libraries(
  crypto_test
  crypto
  GTest::gtest_main
)
gtest_discover_tests(crypto_test)

# create crypto module benchmark binary
add_executable(
  crypto_bench
  crypto_bench.cc
)
target_link_libraries(
  crypto_bench
  crypto
  benchmark::benchmark
)

set_property(TARGET crypto_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
