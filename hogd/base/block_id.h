// block_id.h - process-unique block ID generation.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace hogd {

// The generators that mint block IDs. Each runs in its own process, so they
// cannot share a counter; instead each owns a residue class of the ID space
// and never emits an ID belonging to another.
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
    static std::atomic<uint64_t> counters[kBlockIdLaneCount]{};
    return next(counters[static_cast<uint64_t>(lane)], lane,
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
    const auto residue = static_cast<uint64_t>(lane);
    const uint64_t reading =
        now_ns < 0 ? residue
                   : lane_floor(static_cast<uint64_t>(now_ns), residue);

    uint64_t prev = last.load(std::memory_order_relaxed);
    while (true) {
      // Stepping by the lane count, from a [prev] already in the lane, keeps
      // every ID in its own residue class.
      const uint64_t candidate =
          std::max(reading, lane_floor(prev, residue) + kBlockIdLaneCount);
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

private:
  // Largest value at or below [value] that belongs to the lane [residue].
  static uint64_t lane_floor(uint64_t value, uint64_t residue)
  {
    if (value < residue) {
      return residue;
    }
    return value - ((value - residue) % kBlockIdLaneCount);
  }
};

} // namespace hogd
