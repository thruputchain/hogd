#include "hogd/crypto/sha256.h"

#include <sodium.h> // IWYU pragma: keep

#include <cstddef>
#include <span>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto_error.h"
#include "hogd/crypto/initialize.h"

namespace hogd {

CryptoStatus<Hash>
sha256(std::span<const std::byte, std::dynamic_extent> const &input)
{
  auto init_res = initialize_crypto();
  if (unlikely(!init_res)) {
    return init_res.error();
  }

  Hash out;
  // NOLINTBEGIN(misc-include-cleaner)
  if (unlikely(
          crypto_hash_sha256(out.data(),
                             reinterpret_cast<const uint8_t *>(input.data()),
                             input.size()) != 0)) {
    return CryptoError{CryptoErrorCode::kSHA256Error};
  }
  // NOLINTEND(misc-include-cleaner)

  return out;
}

} // namespace hogd
