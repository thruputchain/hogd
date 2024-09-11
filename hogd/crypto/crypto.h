#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <sodium.h> // IWYU pragma: keep

#include "hogd/base/status.h"

namespace hogd {

using Siphash24Key = std::array<uint8_t, 16>;
// TODO(geoff): replace when x23 ready
using SHA256Hash = std::array<uint8_t, 32>;

/**
 * This function has to be called before
 * any functions within libsodium are called.
 *
 * This is threadsafe and designed to be cheap to call.
 */
GStatus<void> inline initialize_crypto()
{
  // NOLINTBEGIN(misc-include-cleaner)
  if (sodium_init() == -1) {
    // failed to initialize libsodium
    return GError{GCode::kUnknown};
  }
  // NOLINTEND(misc-include-cleaner)

  return {};
}

/**
 * Returns SHA256 of the input span.
 *
 * Libsodium's version of sha256, on current release (1.20) does not return
 * errors.
 */
SHA256Hash inline sha256(std::span<const std::byte> input)
{
  SHA256Hash out;
  // NOLINTBEGIN(misc-include-cleaner)
  [[maybe_unused]]
  int res = crypto_hash_sha256(out.data(),
                               reinterpret_cast<const uint8_t *>(input.data()),
                               input.size());
  // NOLINTEND(misc-include-cleaner)
  assert(res == 0);

  return out;
}

/**
 * SipHash computes a 64-bit message authentication code from a variable-length message and 128-bit secret key (Wikipedia)
 * This function implements the version with parameters 2-4 (industry standard)
 * Output length is 64 bits.
 */
uint64_t inline siphash24(std::span<const std::byte> input,
                          Siphash24Key const &key)
{

  // NOLINTBEGIN(misc-include-cleaner)
  static_assert(sizeof(Siphash24Key) == crypto_shorthash_siphash24_KEYBYTES);
  static_assert(sizeof(uint64_t) == crypto_shorthash_siphash24_BYTES);
  // NOLINTEND(misc-include-cleaner)

  uint64_t out;

  // This is safe on nullptr (0-length) inputs:
  // Verifiable by reading the implementation at
  // https://github.com/jedisct1/libsodium/blob/master/src/libsodium/crypto_shorthash/siphash24/ref/shorthash_siphash24_ref.c#L6
  // This function also never returns anything other than 0, so the error check
  // is redundant.

  // NOLINTBEGIN(misc-include-cleaner)
  [[maybe_unused]]
  int res = crypto_shorthash_siphash24(reinterpret_cast<uint8_t *>(&out),
                                       reinterpret_cast<const uint8_t *>(
                                           input.data()),
                                       input.size(), key.data());
  // NOLINTEND(misc-include-cleaner)
  assert(res == 0);

  return out;
}

Siphash24Key inline siphash24_keygen()
{
  Siphash24Key out;
  // Both siphash24 and siphash24x in libsodium use a 16-byte key,
  // and use the same keygen method.
  // https://libsodium.gitbook.io/doc/hashing/short-input_hashing
  crypto_shorthash_keygen(out.data()); // NOLINT(misc-include-cleaner)
  return out;
}

} // namespace hogd
