#include "hogd/crypto/initialize.h"

#include <sodium.h> // IWYU pragma: keep

#include "hogd/base/status.h"

namespace hogd {

GStatus<void>
initialize_crypto()
{
  // It's safe to call sodium_init concurrently.
  // This function has to be called to completion before ANY call to ANY sodium
  // method begins

  // NOLINTBEGIN(misc-include-cleaner)
  if (sodium_init() == -1) {
    // failed to initialize libsodium
    return GError{GCode::kUnknown};
  }
  // NOLINTEND(misc-include-cleaner)

  return {};
}

} // namespace hogd
