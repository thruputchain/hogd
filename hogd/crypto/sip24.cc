#include "hogd/crypto/sip24.h"

#include <sodium.h> // IWYU pragma: keep

#include <cstdint>
#include <stdexcept>

namespace hog {

uint64_t
sip24(const uint8_t *data_in, uint32_t data_in_len, Sip24Key const &key)
{
  // NOLINTBEGIN(misc-include-cleaner)
  static_assert(sizeof(Sip24Key) == crypto_shorthash_KEYBYTES);
  static_assert(sizeof(uint64_t) == crypto_shorthash_BYTES);
  // NOLINTEND(misc-include-cleaner)

  uint64_t out;

  // NOLINTNEXTLINE
  if (crypto_shorthash(reinterpret_cast<uint8_t *>(&out), data_in, data_in_len,
                       key.data()) != 0) {
    throw std::runtime_error("shorthash failed!");
  }

  return out;
}

Sip24Key
sip24_keygen()
{
  Sip24Key out;
  crypto_shorthash_keygen(out.data()); // NOLINT
  return out;
}

} // namespace hog
