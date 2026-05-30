#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>

#include "types.hh" // RuntimeMemoryLength

namespace hogd {

// Copies as many leading bytes of `src` as fit into `dst`, leaving the rest of
// `dst` untouched, and returns the FULL length of `src` (NOT the number of
// bytes copied). A returned value greater than `dst.size()` means the caller's
// buffer was too small and the output was truncated.
inline RuntimeMemoryLength
copy_to_guest_buffer(std::span<std::byte> dst, std::span<const std::byte> src)
{
  size_t copy_len = std::min(dst.size(), src.size());
  if (copy_len > 0) {
    assert(dst.data() != nullptr);
    assert(src.data() != nullptr);
    std::memcpy(dst.data(), src.data(), copy_len);
  }
  // Every syscall payload routed through here is bounded well below
  // UINT32_MAX by protocol max-lengths; assert the invariant so a future
  // caller with a larger payload can't silently wrap the reported length.
  assert(src.size() <= std::numeric_limits<RuntimeMemoryLength>::max());
  return static_cast<RuntimeMemoryLength>(src.size());
}

} // namespace hogd
