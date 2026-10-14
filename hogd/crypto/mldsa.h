// mldsa.h - ML-DSA-65 (FIPS 204) signature verification
//
// ML-DSA is the module-lattice-based signature scheme of FIPS 204. hogd uses
// its ML-DSA-65 parameter set. The implementation is mldsa-native (the
// external/mldsa-native submodule), built for production without key
// generation or signing; tests that need to make signatures use
// mldsa_signer.h.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "x23/vec.h"

namespace hogd {

// Sizes in bytes of ML-DSA-65 public keys and signatures (FIPS 204, Table 2).
constexpr size_t kMlDsa65PublicKeyBytes = 1952;
constexpr size_t kMlDsa65SignatureBytes = 3309;

// kMlDsaMaxContextBytes is the length of the longest context string ML-DSA
// accepts.
constexpr size_t kMlDsaMaxContextBytes = 255;

// Every byte string of a public key's length decodes to some public key, but
// not every byte string of a signature's length is a well-formed signature;
// mldsa65_verify rejects malformed ones like any other invalid signature.
using MlDsa65PublicKey = std::array<uint8_t, kMlDsa65PublicKeyBytes>;
using MlDsa65Signature = std::array<uint8_t, kMlDsa65SignatureBytes>;

// mldsa65_verify returns whether sig is a valid ML-DSA-65 signature on msg
// under public_key with the context string ctx, as computed by ML-DSA.Verify
// (FIPS 204, Algorithm 3): pure ML-DSA, not HashML-DSA. A ctx longer than
// kMlDsaMaxContextBytes never verifies.
//
// FIPS 204 leaves verifiers no choices, malformed encodings included, so the
// result depends only on the arguments. Signatures are not unique, though:
// a signer can make any number of valid signatures on the same message, so
// signature bytes must not identify anything.
[[nodiscard]] bool mldsa65_verify(const MlDsa65PublicKey &public_key,
                                  const MlDsa65Signature &sig, x23::BufView msg,
                                  x23::BufView ctx = {});

} // namespace hogd
