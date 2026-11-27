/* Compiles the x23c --c output for c_backend.x as freestanding C11, with
 * X23_MALLOC standing in for libc's malloc. */

#include <stddef.h>
#include <stdint.h>

void *bump_alloc(size_t n);

#define X23_MALLOC(n) bump_alloc(n)
#include "tests/c_backend.h"

static _Alignas(16) unsigned char heap[1 << 16];
static size_t heap_used;

void *
bump_alloc(size_t n)
{
  size_t start = (heap_used + 15) & ~(size_t)15;
  if (start > sizeof(heap) || n > sizeof(heap) - start)
    return NULL;
  heap_used = start + n;
  return heap + start;
}

x23_status
c_backend_check(const uint8_t *buf, uint32_t len, Everything *out)
{
  heap_used = 0;
  return Everything_from_xdr(buf, len, out);
}
