#include "hogd/base/status.h"
#include "hogd/init.h"

namespace hogd {

// crypto_init must be called before using any crypto functions.
GStatus<void>
crypto_init()
{
  return {};
}

} // namespace hogd
