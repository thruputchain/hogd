// scalable_map.h - provides a scalable hash table

#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <concepts>
#include <functional>
#include <mutex>
#include <ranges>
#include <tuple>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/disk_writer.h"
#include "hogd/ramdb/item_allocator.h"

namespace hogd {

// ScalableMap is a scalable hash table that uses a LogAllocator as its memory
// allocator.
//
// It uses partitioned locking to scale and chaining to resolve collisions.
class ScalableMap {
public:
  ScalableMap(ItemAllocator &allocator, DiskWriter &disk_writer,
              int mutex_order, int bucket_order)
    : allocator_(allocator), disk_writer_(disk_writer),
      old_size_(1UL << bucket_order), expanded_idx_(1UL << bucket_order),
      mutexes_(1UL << mutex_order), item_counts_(1UL << bucket_order),
      buckets_(1UL << bucket_order)
  {
    assert(bucket_order >= mutex_order);
  }
  ~ScalableMap() = default;

  // Disable move and copy.
  ScalableMap(ScalableMap &&map) = delete;
  ScalableMap &operator=(ScalableMap &&map) = delete;
  ScalableMap(const ScalableMap &map) = delete;
  ScalableMap &operator=(const ScalableMap &map) = delete;

  // get finds an item for a given key.
  DBStatus<DBItem> get(std::span<const std::byte> key);

  // get_if_type finds an item of a specific type for a given key. If the
  // type or the size of val_out do not match exactly, it will return
  // DBCode::kTypeMismatch.
  DBStatus<void> get_if_type(std::span<const std::byte> key, uint8_t type,
                             std::span<std::byte> val_out);

  // put inserts an item. If the item's key already exists, the item will be
  // replaced.
  DBStatus<void> put(std::span<const std::byte> key, uint8_t type,
                     std::span<const std::byte> val);

  // remove deletes an item. It is not considered an error if the item does not
  // exist.
  DBStatus<void> remove(std::span<const std::byte> key);

  // apply_change applies an item change from the on-disk log. It is assumed
  // this will be called from a single thread, so locking is not required.
  DBStatus<void> apply_change(ItemHeader *hdr);

  // prepare_expand prepares to increase the size of the map. It must be called
  // without concurrent access. The expansion itself is performed by do_expand,
  // which should only be called if the return value is true.
  bool prepare_expand();

  // do_expand is concurrent and does not pause operations on the map. It should
  // be called after prepare_expand to finish expanding.
  void do_expand();

  // modify_locked finds an item for a given key, and runs fn on it with
  // the bucket lock held. It returns true if the item was found.
  //
  // This method shouldn't normally be called directly, but it is useful for log
  // compaction.
  bool modify_locked(std::span<const std::byte> key,
                     const std::function<void(ItemHeader **h)> &fn);

  // size returns an estimate of the number of elements in the map.
  size_t size() const
  {
    return std::ranges::fold_left(
        item_counts_, 0, [](size_t sum, const std::atomic<uint16_t> &cnt) {
          return sum + cnt.load(std::memory_order_relaxed);
        });
  }

  // for_each executes func on each element.
  void for_each(std::invocable<ItemHeader *> auto func)
  {
    for (size_t i = 0; i < buckets_.size();) {
      // retry if this bucket still needs to be expanded.
      if (i < old_size_ && i >= expanded_idx_.load(std::memory_order_acquire)) {
        cpu_relax();
        continue;
      }

      // otherwise, it's safe to loop through its items.
      std::scoped_lock guard(mutexes_[i & (mutexes_.size() - 1)]);
      for (ItemHeader *hdr = buckets_[i]; hdr != nullptr; hdr = hdr->next) {
        func(hdr);
      }
      ++i;
    }
  }

  // for_each_parallel executes func in parallel on each element using a
  // WorkerPool.
  void for_each_parallel(std::invocable<ItemHeader *> auto func,
                         WorkerPool &pool)
  {
    constexpr size_t kBucketsPerJob = 32;
    pool.for_each_parallel(
        std::ranges::iota_view{0UL, buckets_.size()} |
            std::views::transform([this](size_t i) {
              // wait for expansion of this bucket to finish.
              while (i < old_size_ &&
                     i >= expanded_idx_.load(std::memory_order_acquire)) {
                cpu_relax();
              }
              return i;
            }),
        [this, func](size_t i) {
          std::scoped_lock guard(mutexes_[i & (mutexes_.size() - 1)]);
          for (ItemHeader *hdr = buckets_[i]; hdr != nullptr; hdr = hdr->next) {
            func(hdr);
          }
        },
        kBucketsPerJob);
  }

private:
  // used to allocate the items in the map.
  ItemAllocator &allocator_;
  // used to log changes to disk.
  DiskWriter &disk_writer_;
  // the number of buckets before a resizing.
  size_t old_size_;
  // the buckets expanded so far during resizing.
  std::atomic<size_t> expanded_idx_;
  // locks to protect the buckets (not 1:1 with buckets_).
  std::vector<std::mutex> mutexes_;
  // the number of items per lock. used to calculate the load factor.
  std::vector<std::atomic<uint16_t>> item_counts_;
  // the buckets for storing items (indexed by hash value).
  std::vector<ItemHeader *> buckets_;

  // acquire_bucket uses the hash of the key to return the appropriate lock
  // (with it held), bucket, and item counter.
  inline std::tuple<std::unique_lock<std::mutex>, ItemHeader **,
                    std::atomic<uint16_t> &>
  acquire_bucket(size_t hash);

  // unlink removes an item from a bucket and frees it.
  inline void unlink(ItemHeader **item_ptr);
};

} // namespace hogd
