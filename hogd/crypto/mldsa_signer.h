// mldsa_signer.h - ML-DSA-65 (FIPS 204) key generation and signing, for tests
//
// hogd only verifies ML-DSA signatures (mldsa.h); this lets tests and
// benchmarks make them. It needs the mldsa_signer library, which production
// targets must not link.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "hogd/crypto/mldsa.h"
#include "x23/vec.h"

namespace hogd {

// Size in bytes of an ML-DSA-65 secret key (FIPS 204, Table 2).
constexpr size_t kMlDsa65SecretKeyBytes = 4032;

// MlDsaSeed is the seed key generation derives a key pair from (xi in
// FIPS 204, Algorithm 6).
using MlDsaSeed = std::array<uint8_t, 32>;

// MlDsaRnd is the per-signature randomness of ML-DSA.Sign (rnd in FIPS 204,
// Algorithm 2).
using MlDsaRnd = std::array<uint8_t, 32>;

// MlDsa65SigningKey is an ML-DSA-65 key pair. It cannot be copied, and it
// zeroes its secret key when destroyed or moved from.
class MlDsa65SigningKey {
public:
  // generate makes a key pair from a fresh random seed (ML-DSA.KeyGen).
  static MlDsa65SigningKey generate();

  // from_seed derives the key pair for seed (ML-DSA.KeyGen_internal), as the
  // ACVP and Wycheproof key generation vectors do.
  static MlDsa65SigningKey from_seed(const MlDsaSeed &seed);

  MlDsa65SigningKey(MlDsa65SigningKey &&other) noexcept;
  MlDsa65SigningKey(const MlDsa65SigningKey &) = delete;
  MlDsa65SigningKey &operator=(const MlDsa65SigningKey &) = delete;
  MlDsa65SigningKey &operator=(MlDsa65SigningKey &&) = delete;
  ~MlDsa65SigningKey();

  const MlDsa65PublicKey &public_key() const { return public_key_; }

  // sign signs msg with the context string ctx as ML-DSA.Sign (FIPS 204,
  // Algorithm 2) does, hedged with fresh randomness: signing the same message
  // twice gives two different valid signatures. Returns std::nullopt if ctx
  // is longer than kMlDsaMaxContextBytes.
  std::optional<MlDsa65Signature> sign(x23::BufView msg,
                                       x23::BufView ctx = {}) const;

  // sign_with_rnd is sign with the per-signature randomness supplied. An
  // all-zero rnd gives deterministic ML-DSA (FIPS 204, Section 3.4), and any
  // fixed rnd makes signatures reproducible, e.g. to compare them byte for
  // byte with another implementation's.
  std::optional<MlDsa65Signature>
  sign_with_rnd(x23::BufView msg, x23::BufView ctx, const MlDsaRnd &rnd) const;

private:
  MlDsa65SigningKey() = default;

  MlDsa65PublicKey public_key_{};
  std::array<uint8_t, kMlDsa65SecretKeyBytes> secret_key_{};
};

} // namespace hogd
