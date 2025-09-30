# create crypto module library
# Note: rust crypto is now built via the main rust workspace

add_library(crypto_interface INTERFACE)
target_link_libraries(crypto_interface
  INTERFACE
  protocol
  crc32c
  blake3
  OpenSSL::SSL
  OpenSSL::Crypto
)

target_include_directories(crypto_interface
  INTERFACE
  ${CMAKE_BINARY_DIR}
)

add_dependencies(
  crypto_interface 
  generate_crypto_ffi_h
)

add_library(crypto_objects OBJECT crypto.cc)
target_link_libraries(crypto_objects crypto_interface)

add_library(
  crypto
  $<TARGET_OBJECTS:crypto_objects>
)

target_include_directories(crypto
  INTERFACE
  ${CMAKE_BINARY_DIR}
)

find_package(OpenSSL REQUIRED)
target_link_libraries(
  crypto
  protocol
  crc32c
  OpenSSL::SSL
  OpenSSL::Crypto
  blake3
  unified_lib
)

add_library(crypto_test_objects OBJECT crypto_test.cc)
target_link_libraries(
  crypto_test_objects 
  crypto_interface
  OpenSSL::SSL
  OpenSSL::Crypto
  GTest::gtest
)

# create crypto module test binary
add_executable(
  crypto_test
  $<TARGET_OBJECTS:crypto_test_objects>
)

target_link_libraries(
  crypto_test
  crypto
  GTest::gtest
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
