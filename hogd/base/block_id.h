// block_id.h - process-unique block ID generation.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace hogd {

// Each generator runs in its own process, so they cannot share a counter;
// each owns one residue class of the ID space instead.
enum class BlockIdLane : uint64_t {
  // [MempoolCoordinatorRpc] nomination and validation blocks.
  kMempool = 0,
  // [HogDbExecutorRpc] viewing snapshots.
  kViewSnapshot = 1,
};

// How many residue classes the ID space is split into. Widening it is safe:
// block IDs live only for the lifetime of the process that minted them.
constexpr uint64_t kBlockIdLaneCount = 2;

// Hands out the IDs that key a block's DB snapshot, event block, and executor
// state.
class BlockIdGenerator {
public:
  // Generates a new block ID for [lane]: wall-clock nanoseconds, unique for
  // the lifetime of this process even if the host clock steps backward, and
  // never equal to an ID minted for a different lane. Thread-safe.
  static uint64_t generate(BlockIdLane lane)
  {
    // One counter serves every lane, so hog-devnode, which hosts both
    // servers, still hands each base out once.
    static std::atomic<uint64_t> last = 0;
    return next(last, lane,
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
  }

  // Returns the next ID in [lane] after [last], given clock reading [now_ns].
  // Exposed so tests can drive the clock.
  //
  // [now_ns] is signed so a pre-epoch reading clamps to the bottom of the ID
  // range rather than wrapping to the top, which would strand the counter.
  static uint64_t next(std::atomic<uint64_t> &last, BlockIdLane lane,
                       int64_t now_ns)
  {
    // [last] holds a base, always a multiple of the lane count; stepping by
    // that count and adding the lane keeps every ID in its own residue class.
    const uint64_t ns = now_ns < 0 ? 0 : static_cast<uint64_t>(now_ns);
    const uint64_t reading = ns - (ns % kBlockIdLaneCount);

    uint64_t prev = last.load(std::memory_order_relaxed);
    while (true) {
      const uint64_t base = std::max(reading, prev + kBlockIdLaneCount);
      if (last.compare_exchange_weak(
              /*expected=*/prev,
              /*desired=*/base,
              /*order=*/std::memory_order_relaxed)) {
        return base + static_cast<uint64_t>(lane);
      }
    }
  }
};

} // namespace hogd
