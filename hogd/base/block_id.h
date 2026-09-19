// block_id.h - process-unique block ID generation.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace hogd {

// Hands out the IDs that key a block's DB snapshot, event block, and executor
// state.
class BlockIdGenerator {
public:
  // Generates a new block ID: wall-clock nanoseconds, unique for the lifetime
  // of this process even if the host clock steps backward. Thread-safe.
  static uint64_t generate()
  {
    static std::atomic<uint64_t> last = 0;
    return next(last, std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count());
  }

  // Returns the next ID after [last], given clock reading [now_ns]. Exposed
  // so tests can drive the clock.
  //
  // [now_ns] is signed so a pre-epoch reading clamps to the bottom of the ID
  // range rather than wrapping to the top, which would strand the counter.
  static uint64_t next(std::atomic<uint64_t> &last, int64_t now_ns)
  {
    const uint64_t reading = now_ns < 0 ? 0 : static_cast<uint64_t>(now_ns);
    uint64_t prev = last.load(std::memory_order_relaxed);
    while (true) {
      const uint64_t candidate = std::max(reading, prev + 1);
      // A failed exchange reloads [prev], so the retry's candidate accounts
      // for whatever ID the winning thread just published.
      if (last.compare_exchange_weak(
              /*expected=*/prev,
              /*desired=*/candidate,
              /*order=*/std::memory_order_relaxed)) {
        return candidate;
      }
    }
  }
};

} // namespace hogd
