#include "hogd/crypto/sha256.h"

#include <sodium.h> // IWYU pragma: keep

#include <cassert>
#include <cstddef>
#include <span>

#include "hogd/base/types.h"

namespace hogd {

SHA256Hash
sha256(std::span<const std::byte> input)
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

} // namespace hogd
