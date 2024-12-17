// scalable_map.h - provides a scalable hash table

#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <functional>
#include <mutex>
#include <tuple>
#include <vector>

#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/log_allocator.h"

namespace hogd {

// ScalableMap is a scalable hash table that uses a LogAllocator as its memory
// allocator.
//
// It uses partitioned locking to scale and chaining to resolve collisions.
class ScalableMap {
public:
  ScalableMap(LogAllocator &allocator, int mutex_order, int bucket_order)
    : allocator_(allocator), old_size_(1UL << bucket_order),
      expanded_idx_(1UL << bucket_order), mutexes_(1UL << mutex_order),
      item_counts_(1UL << bucket_order), buckets_(1UL << bucket_order)
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

  // put inserts an item. If replace is false, it will return DBCode::kExists
  // if an item already exists for the given key.
  DBStatus<void> put(std::span<const std::byte> key, uint8_t type,
                     std::span<const std::byte> val, bool replace = true);

  // remove deletes an item. It is not considered an error if the item does not
  // exist.
  DBStatus<void> remove(std::span<const std::byte> key);

  // prepare_expand prepares to increase the size of the map. It must be called
  // without concurrent access. The expansion itself is performed by do_expand,
  // which should only be called if the return value is true.
  bool prepare_expand(int bucket_order);

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

private:
  // the map uses this to allocate key-value pairs.
  LogAllocator &allocator_;
  // the number of buckets before a resizing.
  size_t old_size_;
  // the buckets expanded so far during resizing.
  std::atomic<size_t> expanded_idx_;
  // locks to protect the buckets (not 1:1 with the buckets).
  std::vector<std::mutex> mutexes_;
  // the number of items per lock. used to calculate the load factor.
  std::vector<std::atomic<uint16_t>> item_counts_;
  // the buckets for storing items (indexed by hash value).
  std::vector<ItemHeader *> buckets_;

  // acquire_bucket uses the hash of the key to return the appropriate lock
  // (with it held), bucket, and item counter.
  inline std::tuple<std::unique_lock<std::mutex>, ItemHeader **,
                    std::atomic<uint16_t> &>
  acquire_bucket(std::span<const std::byte> key);

  // unlink removes an item from a bucket and frees it.
  inline void unlink(ItemHeader **item_ptr);
};

} // namespace hogd
