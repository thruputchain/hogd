/* Compiles the x23c --c output for c_backend.x as freestanding C11. */

#include "tests/c_backend.h"

x23_status
c_backend_check(const uint8_t *buf, uint32_t len, Everything *out)
{
  return Everything_from_xdr(buf, len, out);
}
