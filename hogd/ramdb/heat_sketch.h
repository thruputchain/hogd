// heat_sketch.h - approximate per-item read-hotness tracking.
//
// A count-min sketch over item key hashes, used to classify items hot or cold
// for popularity-driven consolidation (see ItemAllocator::consolidate).
//
// Why off to the side rather than a counter in ItemHeader:
//
//   - ItemHeader is exactly 24 bytes with no padding, so a counter field grows
//     it to 32 and costs 33% of item density -- degrading the very packing the
//     consolidator is trying to improve.
//   - Stealing spare bits from the storage byte instead breaks three things: the
//     WAL and snapshot format (ScopedDiskWrite::modify clones the header
//     verbatim and DiskReader rejects any storage value above kCommit), the
//     lock-free tombstone probe in ItemAllocator::compact_one (an ordinary
//     read-modify-write racing an atomic_ref on the same byte), and every
//     existing comparison against ItemStorage.
//
// A fixed-size table sidesteps all of it, and its space cost is O(1) in the item
// count rather than O(n) -- which is what lets the design keep ObjecTier's claim
// of adding no per-object memory overhead.
//
// Every operation is a hint. Counts are approximate by construction (hash
// collisions inflate them; lost races under contention deflate them), and a
// wrong temperature only misplaces an item, never corrupts it. Nothing here is
// on a correctness path.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace hogd {

class HeatSketch {
public:
  // Counters saturate here. 8 bits is enough to separate "read once" from "read
  // often", which is the only distinction consolidation needs.
  static constexpr uint8_t kMaxCount = 255;

  // width_order is log2 of the counters per row. Four rows at 2^20 counters is
  // 4 MiB total, independent of how many items the store holds.
  explicit HeatSketch(int width_order = 20, size_t rows = 4)
    : mask_((1UL << width_order) - 1), rows_(rows),
      counters_(rows << width_order)
  {}

  // touch records an access. Called on the read path, so it must stay to a
  // relaxed load and store per row and take no locks.
  void touch(size_t hash)
  {
    for (size_t r = 0; r < rows_; ++r) {
      std::atomic<uint8_t> &c = counters_[slot(hash, r)];
      // Load-check-store rather than fetch_add: a relaxed fetch_add on a uint8_t
      // wraps 255 -> 0, which would turn the hottest items ice cold. Concurrent
      // touches can lose an increment here, which only ever underestimates.
      uint8_t v = c.load(std::memory_order_relaxed);
      if (v < kMaxCount)
        c.store(v + 1, std::memory_order_relaxed);
    }
  }

  // estimate returns the minimum count across rows, the count-min estimator.
  // Collisions can only inflate an individual row, so the minimum is the
  // tightest available upper bound on the true count.
  uint8_t estimate(size_t hash) const
  {
    uint8_t best = kMaxCount;
    for (size_t r = 0; r < rows_; ++r)
      best = std::min(best, counters_[slot(hash, r)].load(
                                std::memory_order_relaxed));
    return best;
  }

  // halve ages every counter. Without it the sketch records "was ever hot"
  // rather than "is hot", and a working set that moves would never be tracked:
  // early hot items would pin the fast tier forever. The caller picks the epoch.
  void halve()
  {
    for (std::atomic<uint8_t> &c : counters_) {
      uint8_t v = c.load(std::memory_order_relaxed);
      if (v != 0)
        c.store(v >> 1, std::memory_order_relaxed);
    }
  }

  // size_bytes reports the fixed footprint, for reporting space overhead.
  size_t size_bytes() const { return counters_.size(); }

private:
  // Row indices come from mixing the key hash with a per-row constant. The
  // incoming hash is already SipHash-quality, so one round of mixing per row is
  // enough to decorrelate the rows from each other.
  size_t slot(size_t hash, size_t row) const
  {
    uint64_t z = hash + (0x9E3779B97F4A7C15ULL * (row + 1));
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return (row << std::countr_zero(mask_ + 1)) | (z & mask_);
  }

  const size_t mask_;
  const size_t rows_;
  std::vector<std::atomic<uint8_t>> counters_;
};

} // namespace hogd
