// crypto.h - cryptographic utilities

#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

#include <sodium.h>

#include "hogd/base/types.h"

#include "hogd/crypto/blake2xb.h"

namespace hogd {

using Siphash24Key = std::array<uint8_t, 16>;
using SHA256Hash = std::array<uint8_t, 32>;

// Implementation of LtHash, the homomorphic hash function
// described and analyzed in:
//
// Securing Update Propagation with Homomorphic Hashing
// Kevin Lewi, Wonho Kim, Ilya Maykov, Stephen Weis
// https://eprint.iacr.org/2019/227.pdf
//
// Not collision-resistant for multisets, due to the relatively
// low modulus (2^16 or 2^32).  But applied to hashsets with unique elements,
// this is not a problem.
//
// The hash function hashes an input element to a bitstring of length
// N * # of bits in the modulus (here, 16 or 32)
// This is interpreted as N elements in Z mod 2^modulus (QType)
// Addition/subtraction follows coordinatewise.
namespace detail {
template<uint16_t N, std::unsigned_integral QType> struct LtHash {
  std::array<QType, N> data_;

  constexpr static uint32_t kByteLen = N * (sizeof(QType));

  // ctor for a set containing one element
  LtHash(std::span<const std::byte> input)
  {
    blake2xb<kByteLen>(input, std::as_writable_bytes(std::span(data_)));
  }

  // ctor for the empty set
  LtHash() = default;

  // clang and gcc both seem to compile to SIMD here
  LtHash &operator+=(LtHash const &other)
  {
    // The folly implementation uses explicit AVX instructions.
    // TODO(geoff) check whether that brings noticeable performance improvement
    for (uint16_t i = 0; i < N; i++) {
      data_[i] += other.data_[i];
    }
    return *this;
  }

  LtHash &operator-=(LtHash const &other)
  {
    for (uint16_t i = 0; i < N; i++) {
      data_[i] -= other.data_[i];
    }
    return *this;
  }

  bool operator==(const LtHash &other) const = default;
};

} // namespace detail

// Parameter settings chosen as in https://eprint.iacr.org/2019/227.pdf,
// Table 1
using LtHash16 = detail::LtHash<1024, uint16_t>;
// LtHash20 = detail::LtHash<1008, uint20_t> not implemented -- that requires an extra modulo step,
// and bit tricks to make a hypothetical uint20_t type efficient
using LtHash32 = detail::LtHash<1024, uint32_t>;

// sha256 returns a SHA256 hash of the input.
inline SHA256Hash
sha256(std::span<const std::byte> input)
{
  SHA256Hash out;

  [[maybe_unused]]
  int res = crypto_hash_sha256(out.data(),
                               reinterpret_cast<const uint8_t *>(input.data()),
                               input.size());

  // libsodium's version of sha256 (on release 1.20) never returns errors.
  assert(res == 0);
  return out;
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

} // namespace hogd
