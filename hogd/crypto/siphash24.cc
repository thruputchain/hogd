#include "hogd/crypto/siphash24.h"

#include <sodium.h> // IWYU pragma: keep

#include <cstddef>
#include <span>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto_error.h"
#include "hogd/crypto/initialize.h"

namespace hogd {

CryptoStatus<uint64_t>
siphash24(std::span<const std::byte, std::dynamic_extent> const &input,
          Siphash24Key const &key)
{
  auto init_res = initialize_crypto();
  if (unlikely(!init_res)) {
    return init_res.error();
  }

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

  // NOLINTNEXTLINE(misc-include-cleaner)
  if (unlikely(crypto_shorthash_siphash24(reinterpret_cast<uint8_t *>(&out),
                                          reinterpret_cast<const uint8_t *>(
                                              input.data()),
                                          input.size(), key.data()) != 0)) {
    return CryptoError{CryptoErrorCode::kSiphash24Error};
  }

  return out;
}

CryptoStatus<Siphash24Key>
siphash24_keygen()
{
  auto init_res = initialize_crypto();
  if (unlikely(!init_res)) {
    return init_res.error();
  }

  Siphash24Key out;
  // Both siphash24 and siphash24x in libsodium use a 16-byte key,
  // and use the same keygen method.
  // https://libsodium.gitbook.io/doc/hashing/short-input_hashing
  crypto_shorthash_keygen(out.data()); // NOLINT(misc-include-cleaner)
  return out;
}

} // namespace hogd
