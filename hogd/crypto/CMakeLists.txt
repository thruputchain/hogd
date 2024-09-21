# create crypto module library
add_library(
  crypto
  crypto.cc
)

# create crypto module test binary
add_executable(
  crypto_test
  crypto_test.cc
)
add_dependencies(crypto_test libsodium)
target_link_libraries(
  crypto_test
  crypto
  protocol
  ${LIBSODIUM_LIBRARIES}
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
  protocol
  ${LIBSODIUM_LIBRARIES}
  benchmark::benchmark
)
add_dependencies(crypto_bench libsodium)
