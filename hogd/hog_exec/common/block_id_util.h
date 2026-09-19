#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace hogd {

namespace detail {

// Returns the ID that follows every ID [last] has already handed out, given
// the clock reading [now_ns], and publishes it to [last]. Split out from
// [generate_block_id] so tests can drive a clock sequence without touching
// the host clock.
//
// The result is [now_ns] whenever the clock has moved past the previous ID,
// and one past the previous ID whenever it has not. A clock that repeats a
// reading or steps backward therefore costs ID density, never uniqueness.
inline uint64_t
next_block_id(std::atomic<uint64_t> &last, uint64_t now_ns)
{
  uint64_t prev = last.load(std::memory_order_relaxed);
  while (true) {
    const uint64_t candidate = std::max(now_ns, prev + 1);
    // A failed exchange reloads [prev], so the retry's candidate accounts for
    // whatever ID the winning thread just published.
    if (last.compare_exchange_weak(
            /*expected=*/prev,
            /*desired=*/candidate,
            /*order=*/std::memory_order_relaxed)) {
      return candidate;
    }
  }
}

} // namespace detail

// Generates a new block ID. This block ID is guaranteed to be unique for
// the lifetime of this process. This function is thread-safe.
//
// IDs follow wall-clock nanoseconds so they stay comparable with the
// timestamps in logs, but uniqueness does not rest on the clock:
// [system_clock] is not steady, and a host whose realtime clock steps
// backward must not be able to reissue an ID that already keys a block's DB
// snapshot, event block, or executor state.
inline uint64_t
generate_block_id()
{
  static std::atomic<uint64_t> last = 0;
  const uint64_t now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  return detail::next_block_id(last, now_ns);
}

} // namespace hogd
