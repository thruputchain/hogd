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
// uses to maintain these single pointers and synchronize updates to item
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

#include <cstddef>
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

  // release hands the current head to the compactor without taking a
  // replacement, so the next get() starts a fresh loglet. Unlike roll_over it
  // cannot block, and it leaves nothing allocated to a thread that never writes
  // again.
  void release(ItemAllocator &allocator);

private:
  UniqueLoglet head_;
  ItemAllocator *allocator_;
};

// CompactDests is a pair of relocation destinations, one per temperature.
// Keeping two and choosing by temperature is the whole of the relocation
// mechanism: the contract is unchanged and only the choice of which loglet
// receives the clone differs, so a freshly written loglet ends up homogeneous.
//
// A destination deliberately outlives a single compact_one call, so a partly
// filled one carries over to the next. Where it lives decides how well
// consolidation works: a destination that is abandoned between passes leaves the
// hot items of each pass in their own sparse loglet, which is the opposite of
// consolidating them.
struct CompactDests {
  UniqueLoglet cold;
  UniqueLoglet hot;
};

// CompactorStats stores statistics about log compaction.
struct CompactionStats {
  size_t items;
  size_t live_items;
  // net loglets released: sources freed minus destinations taken. Signed
  // because a pass can legitimately take more than it frees -- as an unsigned
  // count that wraps, and the wrapped value reads as an enormous amount of
  // memory reclaimed.
  ptrdiff_t loglets_freed;

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
  {
    // Pre-reserve to the maximum possible occupancy (every loglet in the
    // region queued at once is the upper bound). With this, give_to_compactor()
    // and compact()'s defer-on-bad_alloc recovery can both emplace_back
    // without ever growing the vector — so neither path can throw bad_alloc.
    compactable_loglets_.reserve(allocator_.loglet_count());
  }
  // Hands any partly filled consolidation destination back rather than letting
  // it destruct, which would return a loglet holding live items to the free
  // list. Same reason ItemHead's destructor does it for a write head.
  ~ItemAllocator() { flush_consolidation_dests(); }

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

  // consolidate separates hot from cold items rather than live from dead.
  //
  // It selects victims by *mixedness* instead of fragmentation: a loglet whose
  // live bytes are split between temperatures is worth reorganizing even when it
  // is perfectly compacted, and compact() will never pick it. Each victim's
  // items are then relocated into one of two destinations by is_hot, so a
  // freshly written loglet is homogeneous in temperature.
  //
  // This matters for workloads whose items are updated in place -- fixed-size
  // values never fragment the log, so compaction never visits them and their
  // layout is otherwise frozen for the lifetime of the store.
  //
  // min_mixedness selects how aggressive to be: 0 reorganizes everything, 0.5
  // only perfectly even splits. max_victims bounds the work in one pass, 0 for
  // no bound. Returns the number of loglets reorganized.
  //
  // Candidates are claimed in small batches rather than all at once, so
  // compactable_loglets_ is never emptied and a concurrent compact() can still
  // find work. A bounded pass leaves the loglets it did not reach in place for
  // the next one.
  size_t consolidate(const std::function<bool(ItemHeader *)> &is_hot,
                     float min_mixedness, size_t max_victims = 0);

  // roll_over_heads hands every thread's partly filled write head to the
  // compactor, making the newest data visible to compact() and consolidate().
  // Without it a head reaches the compactable set only when it fills, so up to
  // one loglet per writing thread of the most recently written items can never
  // be selected.
  //
  // PRECONDITION: no put() or remove() may overlap this call -- it moves the
  // heads those paths allocate from. consolidate() already requires this.
  void roll_over_heads();

  // used to steal or return the set of compactible loglets.
  // used by snapshotting to find loglets and pause compaction on them.
  std::vector<UniqueLoglet> take_loglets();

  // give_to_compactor makes a full loglet available to the compactor.
  void give_to_compactor(UniqueLoglet &&ll);

  // flush_consolidation_dests hands the consolidation destinations back to the
  // compactor, ending the run they were accumulating. Call it when no further
  // passes are expected: until then a partly filled destination is deliberately
  // held so the next pass keeps packing into it.
  void flush_consolidation_dests();

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

  // Destinations for consolidation relocations. Owned here rather than in the
  // calling thread for two reasons. Lifetime: Database declares
  // loglet_allocator_ before item_allocator_, so these are released while the
  // loglet allocator is still alive -- which a caller thread's thread_local is
  // not, and that was BUG-3. Effectiveness: they persist across passes, so a
  // sequence of small bounded passes packs hot items into the same loglets that
  // one large pass would, instead of one sparse loglet per pass.
  //
  // Not locked. consolidate() requires that no put()/remove() overlaps it, and
  // is documented as a single serial pass; two concurrent consolidations would
  // race on these.
  detail::CompactDests consolidate_dests_;

  // compact_one is a helper function that compacts a single loglet. When is_hot
  // is non-null, relocations are split across two destinations by temperature
  // instead of all landing in one. dests selects which destination pair to
  // relocate into; null means this thread's own pair, which is what compaction
  // on the worker pool uses.
  CompactionStats
  compact_one(UniqueLoglet &&src,
              const std::function<bool(ItemHeader *)> *is_hot = nullptr,
              detail::CompactDests *dests = nullptr);
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
