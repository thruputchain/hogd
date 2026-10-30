/*
 * ML-DSA-65 verification: mldsa-native's single-file build configured as
 * hogd's mldsa_native_verify library (hogd/crypto/CMakeLists.txt). Key
 * generation and signing are compiled out; only portable C is built.
 *
 * mldsa-native/ is the mldsa/ directory of the external/mldsa-native
 * submodule at 834a90d5e846ffa1e1611bd24e160bb2e9b86d35, without the native
 * (assembly) backends, which hogd does not build.
 */
#define MLD_CONFIG_PARAMETER_SET 65
#define MLD_CONFIG_NAMESPACE_PREFIX hogd_mldsa65
#define MLD_CONFIG_NO_KEYPAIR_API
#define MLD_CONFIG_NO_SIGN_API
#define MLD_CONFIG_NO_RANDOMIZED_API

#include "mldsa-native/mldsa_native.c"
