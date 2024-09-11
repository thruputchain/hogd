add_library(
  crypto
  siphash24.cc
  sha256.cc
  initialize.cc
)

add_dependencies(crypto libsodium)

add_executable(
  crypto_test
  siphash_test.cc
  sha256_test.cc
)
target_link_libraries(
  crypto_test
  crypto
  ${LIBSODIUM_LIBRARIES}
  GTest::gtest_main
)
gtest_discover_tests(crypto_test)
