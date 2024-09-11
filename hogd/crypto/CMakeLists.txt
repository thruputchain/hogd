
add_executable(
  crypto_test
  siphash_test.cc
  sha256_test.cc
)

add_dependencies(crypto_test libsodium)

target_link_libraries(
  crypto_test
  ${LIBSODIUM_LIBRARIES}
  GTest::gtest_main
)
gtest_discover_tests(crypto_test)
