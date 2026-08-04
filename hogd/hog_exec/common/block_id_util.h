#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace hogd {

// Generates a new block ID. This block ID is guaranteed to be unique for
// the lifetime of this process. This function is thread-safe.
inline uint64_t
generate_block_id()
{
  static std::atomic<uint64_t> last_ns = 0;
  while (true) {
    auto now = std::chrono::system_clock::now();
    uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      now.time_since_epoch())
                      .count();
    uint64_t last_ns_relaxed = last_ns.load(std::memory_order_relaxed);
    if (ns == last_ns_relaxed) {
      continue;
    }
    if (last_ns.compare_exchange_weak(
            /*expected=*/last_ns_relaxed,
            /*desired=*/ns,
            /*order=*/std::memory_order_relaxed)) {
      return ns;
    }
  }
}

} // namespace hogd
