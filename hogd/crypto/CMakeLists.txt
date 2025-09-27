# create crypto module library
add_subdirectory(ed25519)
add_subdirectory(siphash24)

add_library(crypto_interface INTERFACE)
add_dependencies(crypto_interface libsodium_build-configure)
#header files for libsodium are added to global include path in toplevel CMakeLists.txt
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
  generate_ed25519_ffi_h
  generate_siphash24_ffi_h
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
add_dependencies(crypto libsodium_build)
target_link_libraries(
  crypto
  protocol
  libsodium
  crc32c
  OpenSSL::SSL
  OpenSSL::Crypto
  blake3
  ed25519_lib
  siphash24_lib
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
