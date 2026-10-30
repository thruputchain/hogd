/*
 * ML-DSA-65 key generation and signing from caller-supplied randomness, for
 * tests: mldsa-native's single-file build configured as hogd's
 * mldsa_native_signer library (hogd/crypto/CMakeLists.txt). Its symbols carry
 * their own namespace prefix, so it links beside the verifier.
 */
#define MLD_CONFIG_PARAMETER_SET 65
#define MLD_CONFIG_NAMESPACE_PREFIX hogd_mldsa65_signer
#define MLD_CONFIG_CORE_API_ONLY
#define MLD_CONFIG_NO_VERIFY_API
#define MLD_CONFIG_NO_RANDOMIZED_API

#include "mldsa-native/mldsa_native.c"
