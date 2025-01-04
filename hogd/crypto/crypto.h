// crypto.h - cryptographic utilities

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include <sodium.h>

#include "hogd/base/types.h"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/x23.h"

namespace hogd {

using Siphash24Key = std::array<uint8_t, 16>;

// sha256 returns a SHA256 hash of the input.
inline SHA256Hash
sha256(const void *data, size_t size)
{
  SHA256Hash out;

  [[maybe_unused]]
  int res = crypto_hash_sha256(out.data(),
                               reinterpret_cast<const uint8_t *>(data), size);

  // libsodium's version of sha256 (on release 1.20) never returns errors.
  assert(res == 0);
  return out;
}

inline SHA256Hash
sha256(x23::is_bytespan auto const &span)
{
  return sha256(span.data(), span.size());
}

inline SHA256Hash
sha256(x23::is_xdr_type auto const &value)
{
  auto buf = x23::serialize(value);
  return sha256(buf);
}

// siphash24 returns a 64-bit message authentication code from a
// variable-length message and 128-bit secret key. This function implements the
// version with parameters 2-4.
inline uint64_t
siphash24(std::span<const std::byte> input, const Siphash24Key &key)
{
  static_assert(sizeof(Siphash24Key) == crypto_shorthash_siphash24_KEYBYTES);
  static_assert(sizeof(uint64_t) == crypto_shorthash_siphash24_BYTES);

  uint64_t out;

  // This is safe on nullptr (0-length) inputs:
  // Verifiable by reading the implementation at
  // https://github.com/jedisct1/libsodium/blob/master/src/libsodium/crypto_shorthash/siphash24/ref/shorthash_siphash24_ref.c#L6
  // This function also never returns anything other than 0, so the error check
  // is redundant.

  [[maybe_unused]]
  int res = crypto_shorthash_siphash24(reinterpret_cast<uint8_t *>(&out),
                                       reinterpret_cast<const uint8_t *>(
                                           input.data()),
                                       input.size(), key.data());
  // libsodium's version of siphash24 (on release 1.20) never returns errors.
  assert(res == 0);
  return out;
}

// siphash24_keygen generates a key for use with siphash24.
inline Siphash24Key
siphash24_keygen()
{
  Siphash24Key out;
  // Both siphash24 and siphash24x in libsodium use a 16-byte key,
  // and use the same keygen method.
  // https://libsodium.gitbook.io/doc/hashing/short-input_hashing
  crypto_shorthash_keygen(out.data());
  return out;
}

inline bool
ed25519_verify(Ed25519PublicKey const &publickey,
               Ed25519Signature const &signature,
               std::span<const std::byte> message)
{
  return crypto_sign_ed25519_verify_detached(signature.data(),
                                             reinterpret_cast<const uint8_t *>(
                                                 message.data()),
                                             message.size(),
                                             publickey.data()) == 0;
}

} // namespace hogd
