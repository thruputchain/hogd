#include "hogd/ramdb/item_allocator.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/string.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/ramdb/constants.h"
#include "hogd/ramdb/loglet.h"
#include "hogd/ramdb/scalable_map.h"

namespace hogd {

namespace detail {

ItemHead::~ItemHead()
{
  try {
    if (head_)
      allocator_->give_to_compactor(std::move(head_));
  } catch (const std::exception &e) {
    // must panic because if give_to_compactor throws, head_ will be freed.
    panic("Couldn't free ItemHead: {}", e.what());
  }
}

inline Loglet *
ItemHead::get(ItemAllocator &allocator, size_t kv_len)
{
  assert(!head_ || allocator_ == &allocator);

  // Try to use the current head.
  if (head_ && head_->bytes_left() >= sizeof(ItemHeader) + kv_len) [[likely]]
    return head_.get();

  // Otherwise the head is full or has not yet been allocated. Allocate
  // a new one. head_ can be nullptr after this if the compactor is unable
  // to find free space.
  roll_over(allocator);

  return head_.get();
}

inline void
ItemHead::roll_over(ItemAllocator &allocator)
{
  allocator_ = &allocator;

  if (head_)
    allocator_->give_to_compactor(std::move(head_));
  head_ = make_unique_loglet(allocator_->allocator_, LogletMode::kMemoryItems);
}

} // namespace detail

ItemHeader *
ItemAllocator::alloc(uint8_t type, size_t key_len, size_t value_len)
{
  Loglet *ll = head_->get(*this, key_len + value_len);
  if (!ll) [[unlikely]]
    return nullptr;

  ItemHeader *hdr = ll->alloc(ItemStorage::kNormal, type, key_len, value_len);
  assert(hdr != nullptr); // cannot fail; we checked the space left.
  return hdr;
}

size_t
ItemAllocator::compact(float utilization_thresh)
{
  // move_if moves the elements for which pred is true to a new vector and
  // returns it. The src vector is modified to contain the remaining elements
  // where pred is false.
  auto move_if = []<typename T, typename Func>(std::vector<T> &src, Func pred) {
    auto tail = std::ranges::partition(src, std::not_fn(pred));
    std::vector<T> ret(std::make_move_iterator(tail.begin()),
                       std::make_move_iterator(tail.end()));
    src.erase(tail.begin(), tail.end());
    return ret;
  };

  // record start time for measuring compaction time.
  ElapsedTimer timer;

  // take all the compactable loglets.
  std::vector<UniqueLoglet> claimed;
  {
    std::scoped_lock guard(lock_);
    claimed = move_if(compactable_loglets_, [utilization_thresh](
                                                const UniqueLoglet &ll) {
      return !ll->fully_compacted() && ll->utilization() <= utilization_thresh;
    });
  }

  // Hand each claimed loglet to a worker. We hold our own shared_ptr in
  // `in_flight` so that if pool_.submit() throws bad_alloc (inside
  // WorkerPool's promise / UniqueFunction / std::queue allocations) after
  // its inner lambda is constructed, the loglet survives the lambda's
  // destruction and we can put it back into compactable_loglets_.
  std::vector<std::shared_ptr<UniqueLoglet>> in_flight;
  std::vector<std::future<CompactionStats>> completions;
  CompactionStats stats{};

  try {
    in_flight.reserve(claimed.size());
    completions.reserve(claimed.size());

    for (auto &ll : claimed) {
      in_flight.push_back(std::make_shared<UniqueLoglet>(std::move(ll)));
      completions.emplace_back(pool_.submit(
          [this, l = in_flight.back()] { return compact_one(std::move(*l)); }));
    }

    // wait for the compactions to finish.
    for (std::future<CompactionStats> &f : completions)
      stats += f.get();
  } catch (const std::bad_alloc &) {
    // Drain any submitted workers so their std::move(*l) settles before
    // we inspect in_flight. compactable_loglets_'s emplace_back below
    // cannot throw because the ItemAllocator constructor reserved capacity
    // to LogletAllocator::loglet_count() — the maximum possible occupancy.
    for (std::future<CompactionStats> &f : completions) {
      try {
        // We call get() only to drain the worker and let its std::move(*l)
        // settle before we inspect in_flight; the [[nodiscard]] stats are
        // intentionally unused here.
        [[maybe_unused]] auto stats = f.get();
      } catch (...) { // NOLINT(bugprone-empty-catch)
        // Drain only — a worker exception here is suppressed because we are
        // already unwinding compact() on bad_alloc and have no useful path
        // to surface it. compact_one is allocation-free post the ctor's
        // reserve, so this catch is not expected to fire in practice.
      }
    }
    std::scoped_lock guard(lock_);
    for (std::shared_ptr<UniqueLoglet> &l : in_flight)
      if (l && *l)
        compactable_loglets_.emplace_back(std::move(*l));
    for (UniqueLoglet &ll : claimed)
      if (ll)
        compactable_loglets_.emplace_back(std::move(ll));
    warn("compact: deferred {} loglets after bad_alloc", claimed.size());
    return 0;
  }

  // print the compaction rate.
  auto seconds = timer.elapsed();
  info("finished compaction in {:.3f} seconds", seconds);
  if (stats.items > 0) {
    // A fast pass can measure zero elapsed time even on a steady clock; guard
    // the division so the rate is never non-finite.
    double items_per_sec =
        seconds > 0.0 ? static_cast<double>(stats.items) / seconds : 0.0;
    info("scanned {:.0f} items/sec ({}% live)", items_per_sec,
         stats.live_items * 100 / stats.items);
  }
  else {
    warn("no items scanned.");
  }

  // determine how much memory was freed.
  size_t free_bytes = stats.loglets_freed * kLogletRegionSize;
  size_t total_bytes = allocator_.size();
  info("reclaimed {}/{} ({}% freed)", print_iec_bytes(free_bytes),
       print_iec_bytes(total_bytes), free_bytes * 100 / total_bytes);
  allocator_.print_stats();
  return free_bytes;
}

detail::CompactionStats
ItemAllocator::compact_one(UniqueLoglet &&src)
{
  CompactionStats stats{};
  size_t bytes_moved = 0;

  // alloc_loglet is a utility for allocating reserved loglets.
  auto alloc_loglet = [this, &stats]() {
    UniqueLoglet dst =
        make_unique_loglet(allocator_, LogletMode::kMemoryItems, true);
    assert(static_cast<bool>(dst));
    stats.loglets_freed--;
    return dst;
  };

  // compact items in this loglet: move live items and skip dead items.
  for (ItemHeader &h : *src) {
    stats.items++;
    ItemHeader *src_item = std::addressof(h);

    // loglet compaction is finished if every live item has been moved.
    if (src->live_size() == bytes_moved)
      break;

    // optimization: skip dead items that can be detected without locking. The
    // atomic_ref must not outlive this check — Loglet::clone(src_item) below
    // performs a plain copy of src_item->storage, and [atomics.ref.generic]/3
    // forbids ordinary access to an object while an atomic_ref to it exists.
    if (std::atomic_ref<ItemStorage>(src_item->storage)
            .load(std::memory_order_acquire) == ItemStorage::kTombstone)
      continue;

    // try to move the item to the new loglet.
    [[maybe_unused]] bool found = map_->modify_locked(
        src_item->get_key(),
        [this, alloc_loglet, src_item, &bytes_moved,
         &stats](ItemHeader **hptr, size_t /* hash */) mutable {
          // modify_locked matched by key, not by pointer. A concurrent
          // different-sized put or remove+put may have already unlinked
          // src_item and inserted a new same-key header; unlink()
          // tombstones src_item under this same bucket lock. *hptr is
          // then the new live header, and cloning src_item over it would
          // clobber the live value with stale bytes. src_item is already
          // dead, so skip it; its memory is reclaimed when this source
          // loglet is freed at the end of the pass.
          if (*hptr != src_item)
            return;

          // initialize the first use of the per-thread loglet.
          static thread_local UniqueLoglet dst;
          if (!dst) [[unlikely]]
            dst = alloc_loglet();

          // allocate the destination item
          ItemHeader *dst_item = dst->clone(src_item);
          if (!dst_item) {
            // loglet is full; get a new one.
            give_to_compactor(std::move(dst));
            dst = alloc_loglet();
            dst_item = dst->clone(src_item);
            assert(dst_item != nullptr);
          }

          // copy the item's key and value to the new location.
          std::ranges::copy(src_item->get_key_and_value(),
                            dst_item->get_key_and_value().begin());

          // point the map to the new location (bucket lock is held).
          dst_item->next = src_item->next;
          *hptr = dst_item;
          stats.live_items++;
          bytes_moved += dst_item->total_bytes();
        });
  }

  stats.loglets_freed++;
  return stats;
}

} // namespace hogd
