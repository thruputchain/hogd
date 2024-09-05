#include "hogd/crypto/blake2b.h"

#include <sodium.h> // IWYU pragma: keep

#include <array>
#include <cstdint>
#include <stdexcept>

namespace hog {

namespace detail {

void
initialize_sodium()
{
  // NOLINTNEXTLINE
  if (sodium_init() == -1) {
    throw std::runtime_error("sodium init failed");
  }
}

} // namespace detail

void
hash_blake2b(std::array<uint8_t, 32> &out, const uint8_t *data_in,
             uint32_t data_in_len)
{
  // NOLINTNEXTLINE
  if (crypto_generichash_blake2b(out.data(), out.size(), data_in, data_in_len,
                                 nullptr, 0) != 0) {
    throw std::runtime_error("crypto hash failed");
  }
}

} // namespace hog
