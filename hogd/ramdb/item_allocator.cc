#include "hogd/ramdb/item_allocator.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
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
  assert(head_ || allocator_ == nullptr);

  // Try to use the current head.
  if (head_ && head_->bytes_left() >= sizeof(ItemHeader) + kv_len) [[likely]]
    return head_.get();

  // Otherwise the head is full or has not yet been allocated. Allocate
  // a new one.
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
  std::vector<UniqueLoglet> loglets;
  {
    std::scoped_lock guard(lock_);
    loglets = move_if(compactable_loglets_, [utilization_thresh](
                                                const UniqueLoglet &ll) {
      return !ll->fully_compacted() && ll->utilization() <= utilization_thresh;
    });
  }

  // compact all of them in parallel.
  std::vector<std::future<CompactionStats>> completions;
  completions.reserve(loglets.size());
  for (auto &ll : loglets) {
    completions.emplace_back(pool_.submit([this, ll = std::move(ll)] mutable {
      return compact_one(std::move(ll));
    }));
  }

  // wait for the compactions to finish.
  CompactionStats stats{};
  for (std::future<CompactionStats> &f : completions)
    stats += f.get();

  // print the compaction rate.
  auto seconds = timer.elapsed();
  info("finished compaction in {:.3f} seconds", seconds);
  if (stats.items > 0) {
    info("scanned {:.0f} items/sec ({}% live)",
         static_cast<double>(stats.items) / seconds,
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

    // optimization: skip dead items that can be detected without locking.
    auto storage = std::atomic_ref<ItemStorage>(src_item->storage);
    if (storage.load(std::memory_order_acquire) == ItemStorage::kTombstone)
      continue;

    // try to move the item to the new loglet.
    [[maybe_unused]] bool found = map_->modify_locked(
        src_item->get_key(),
        [this, alloc_loglet, src_item, &bytes_moved, &stats,
         &storage](ItemHeader **hptr, size_t /* hash */) mutable {
          // recheck if the item became dead before the lock was held.
          if (storage.load(std::memory_order_relaxed) ==
              ItemStorage::kTombstone) [[unlikely]] {
            return;
          }

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
          assert(*hptr == src_item);
          dst_item->next = src_item->next;
          *hptr = dst_item;
          stats.live_items++;
          bytes_moved += dst_item->total_bytes();
        });
    assert(found ||
           storage.load(std::memory_order_relaxed) == ItemStorage::kTombstone);
  }

  stats.loglets_freed++;
  return stats;
}

void
ItemAllocator::roll_heads()
{
  std::unique_lock ul = head_.lock_threads();
  head_.for_each(ul, [this](detail::ItemHead &head) { head.roll_over(*this); });
}

} // namespace hogd
