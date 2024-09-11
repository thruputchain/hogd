#include "hogd/crypto/siphash24.h"

#include <sodium.h> // IWYU pragma: keep

#include <cassert>
#include <cstddef>
#include <span>

#include "hogd/base/types.h"

namespace hogd {

uint64_t
siphash24(std::span<const std::byte> input, Siphash24Key const &key)
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

Siphash24Key
siphash24_keygen()
{
  Siphash24Key out;
  // Both siphash24 and siphash24x in libsodium use a 16-byte key,
  // and use the same keygen method.
  // https://libsodium.gitbook.io/doc/hashing/short-input_hashing
  crypto_shorthash_keygen(out.data()); // NOLINT(misc-include-cleaner)
  return out;
}

} // namespace hogd
