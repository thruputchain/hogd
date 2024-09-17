#include <sodium/core.h>

#include "hogd/base/status.h"
#include "hogd/init.h"

namespace hogd {

// crypto_init must be called before using any crypto functions.
GStatus<void>
crypto_init()
{
  if (sodium_init() == -1)
    return GError{GCode::kUnknown};
  return {};
}

} // namespace hogd
