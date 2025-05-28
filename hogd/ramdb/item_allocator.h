// item_allocator.h - a memory allocator that uses a log and compaction
//
// This design is inspired by the paper "Log-structured Memory for DRAM-based
// Storage" at Fast'14.
//
// A log-based memory allocator reduces memory fragmentation by storing items
// contiguously instead of placing them in different segments based on their
// size. It is a moving allocator that compacts items to reclaim space.
// Therefore, only a single pointer can be maintained to each item and these
// pointers must be updated during compaction. Compaction is done in
// coordination with a ScalableMap, a type of hash table that the log compactor
// uses to maintain these single pointers and sychronize updates to item
// locations.
//
// The log is divided into several fixed-sized segments called loglets. Each
// loglet contains contiguous memory of variably-sized key-value pairs. Each
// loglet can be independently and concurrently compacted.
//
// TODO(amb): One downside to log-based allocators is that they have to move
// more bytes when items are large. Therefore, we will eventually need to
// maintain a separate non-moving region to store large items so they can be
// referenced indirectly in the log.

#pragma once

#include <vector>

#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/ramdb/loglet.h"

namespace hogd {

// forward declarations
class ScalableMap;
class ItemAllocator;

namespace detail {

// ItemHead is used to allocate loglets and manage their lifetime. One is
// maintained for each thread using each ItemAllocator.
class ItemHead {
public:
  ItemHead() = default;
  ~ItemHead();

  // Enable move and disable copy.
  ItemHead(ItemHead &&h) noexcept
    : head_(std::move(h.head_)),
      allocator_(std::exchange(h.allocator_, nullptr))
  {}
  ItemHead &operator=(ItemHead &&h) noexcept
  {
    head_ = std::move(h.head_);
    allocator_ = std::exchange(h.allocator_, nullptr);
    return *this;
  }
  ItemHead(const ItemHead &h) = delete;
  ItemHead &operator=(const ItemHead &h) = delete;

  // returns a loglet with enough space for an item with kv_len.
  Loglet *get(ItemAllocator &allocator, size_t kv_len);
  void roll_over(ItemAllocator &allocator);

private:
  UniqueLoglet head_;
  ItemAllocator *allocator_;
};

// CompactorStats stores statistics about log compaction.
struct CompactionStats {
  size_t items;
  size_t live_items;
  size_t loglets_freed;

  CompactionStats &operator+=(const CompactionStats &stats)
  {
    items += stats.items;
    live_items += stats.live_items;
    loglets_freed += stats.loglets_freed;
    return *this;
  }
};

} // namespace detail

// ItemAllocator provides item allocation through a log-based memory allocator.
class ItemAllocator {
public:
  friend detail::ItemHead;

  ItemAllocator(LogletAllocator &allocator, WorkerPool &pool, ScalableMap *map)
    : allocator_(allocator), pool_(pool), map_(map)
  {}
  ~ItemAllocator() = default;

  // Disable move and copy.
  ItemAllocator(ItemAllocator &&la) = delete;
  ItemAllocator &operator=(ItemAllocator &&la) = delete;
  ItemAllocator(const ItemAllocator &la) = delete;
  ItemAllocator &operator=(const ItemAllocator &la) = delete;

  // alloc allocates an item.
  //
  // WARNING: Can block while waiting for memory to be freed, so must be called
  // without holding locks needed for compaction.
  ItemHeader *alloc(uint8_t type, size_t key_len, size_t value_len);

  // free deletes an item. Usually, the underlying memory will be reclaimed
  // later during compaction.
  //
  // However, if Rewind is true, the bump allocator can be decremented instead
  // of placing a tombstone if the most recent item is freed. This is an
  // optimization that is only safe to enable if the same thread that allocated
  // the item frees it.
  template<bool Rewind = false> void free(ItemHeader *item);

  // compact initiates compaction to reclaim memory from deleted items.
  // Loglets with utilization below utilization_thresh will be compacted. Using
  // a larger fraction will reclaim more memory at the cost of more CPU time
  // spent moving items. Returns the bytes that were reclaimed.
  size_t compact(float utilization_thresh);

  // used to steal or return the set of compactible loglets.
  // used by snapshotting to find loglets and pause compaction on them.
  std::vector<UniqueLoglet> take_loglets();

  // give_to_compactor makes a full loglet available to the compactor.
  void give_to_compactor(UniqueLoglet &&ll);

  // force all threads to move to a new loglet. Only allowed while
  // threads are not actively using their heads (e.g. times when
  // sync() is safe on the DB).
  void roll_heads();

private:
  using CompactionStats = detail::CompactionStats;

  // a lock protecting compactable_loglets_;
  std::mutex lock_;
  // loglets that are fully written and can be compacted.
  std::vector<UniqueLoglet> compactable_loglets_;

  // the allocator for loglets.
  LogletAllocator &allocator_;
  // a worker pool that is used for compaction.
  WorkerPool &pool_;
  // the map needed for locking coordination during compaction.
  ScalableMap *map_;
  // each thread gets its own loglet head.
  PerThread<detail::ItemHead> head_;

  // compact_one is a helper function that compacts a single loglet.
  CompactionStats compact_one(UniqueLoglet &&src);
};

template<bool Rewind>
inline void
ItemAllocator::free(ItemHeader *item)
{
  Loglet &ll = allocator_.get_loglet(item);
  ll.free<Rewind>(item);
}

inline void
ItemAllocator::give_to_compactor(UniqueLoglet &&ll)
{
  std::scoped_lock guard(lock_);
  compactable_loglets_.emplace_back(std::move(ll));
}

inline std::vector<UniqueLoglet>
ItemAllocator::take_loglets()
{
  std::vector<UniqueLoglet> q{};
  {
    std::scoped_lock guard{lock_};
    std::swap(compactable_loglets_, q);
  }
  return q;
}

} // namespace hogd
