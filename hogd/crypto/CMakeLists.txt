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

# ML-DSA (FIPS 204) comes from mldsa-native, the external/mldsa-native
# submodule. Upstream ships no build for consumers, so hogd compiles its
# single-file build, mldsa_native.c, configured by MLD_CONFIG_* definitions:
# once for production and once for tests. Only the portable C code is built;
# the assembly backends stay off, so every validator runs the same code
# whatever its -march.
set(MLDSA_NATIVE_DIR ${CMAKE_SOURCE_DIR}/external/mldsa-native/mldsa)

# ML-DSA-65 verification, linked into crypto (mldsa.h). Key generation and
# signing are compiled out.
set(MLDSA_NATIVE_VERIFY_CONFIG
  MLD_CONFIG_PARAMETER_SET=65
  MLD_CONFIG_NAMESPACE_PREFIX=hogd_mldsa65
  MLD_CONFIG_NO_KEYPAIR_API
  MLD_CONFIG_NO_SIGN_API
  MLD_CONFIG_NO_RANDOMIZED_API
)
add_library(mldsa_native_verify STATIC ${MLDSA_NATIVE_DIR}/mldsa_native.c)
target_include_directories(mldsa_native_verify PRIVATE ${MLDSA_NATIVE_DIR})
target_compile_definitions(mldsa_native_verify
  PRIVATE ${MLDSA_NATIVE_VERIFY_CONFIG}
)

# ML-DSA-65 key generation and signing from caller-supplied randomness, for
# tests and benchmarks only (mldsa_signer.h). Production targets must not link
# this.
set(MLDSA_NATIVE_SIGNER_CONFIG
  MLD_CONFIG_PARAMETER_SET=65
  MLD_CONFIG_NAMESPACE_PREFIX=hogd_mldsa65_signer
  MLD_CONFIG_CORE_API_ONLY
  MLD_CONFIG_NO_VERIFY_API
  MLD_CONFIG_NO_RANDOMIZED_API
)
add_library(mldsa_native_signer STATIC ${MLDSA_NATIVE_DIR}/mldsa_native.c)
target_include_directories(mldsa_native_signer PRIVATE ${MLDSA_NATIVE_DIR})
target_compile_definitions(mldsa_native_signer
  PRIVATE ${MLDSA_NATIVE_SIGNER_CONFIG}
)

add_library(crypto_objects OBJECT crypto.cc mldsa.cc)
target_link_libraries(crypto_objects crypto_interface)
# mldsa.cc includes mldsa_native.h, which must see mldsa_native_verify's
# configuration.
target_include_directories(crypto_objects PRIVATE ${MLDSA_NATIVE_DIR})
target_compile_definitions(crypto_objects
  PRIVATE ${MLDSA_NATIVE_VERIFY_CONFIG}
)

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
  mldsa_native_verify
  unified_lib
)

# ML-DSA key generation and signing (mldsa_signer.h), for tests and benchmarks
# only. Production targets must not link this.
add_library(mldsa_signer STATIC mldsa_signer.cc)
target_include_directories(mldsa_signer PRIVATE ${MLDSA_NATIVE_DIR})
target_compile_definitions(mldsa_signer PRIVATE ${MLDSA_NATIVE_SIGNER_CONFIG})
target_link_libraries(
  mldsa_signer
  crypto
  crypto_interface
  mldsa_native_signer
)

add_library(
  crypto_test_objects OBJECT 
  crypto_test.cc
  ed25519_edge_cases.cc
  mldsa_test.cc
)
target_link_libraries(
  crypto_test_objects 
  crypto_interface
  OpenSSL::SSL
  OpenSSL::Crypto
  GTest::gtest
)
target_compile_definitions(
  crypto_test_objects
  PRIVATE
  HOGD_WYCHEPROOF_DIR="${CMAKE_SOURCE_DIR}/external/wycheproof/testvectors_v1"
)

# create crypto module test binary
add_executable(
  crypto_test
  $<TARGET_OBJECTS:crypto_test_objects>
)

target_link_libraries(
  crypto_test
  hogd
  mldsa_signer
  GTest::gtest
)
hogd_discover_tests(crypto_test)

if(BUILD_BENCHMARKS)
  # create crypto module benchmark binary
  add_executable(
    crypto_bench
    crypto_bench.cc
  )
  target_link_libraries(
    crypto_bench
    crypto
    mldsa_signer
    benchmark::benchmark
  )

  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET crypto_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endif()
