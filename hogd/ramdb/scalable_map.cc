// scalable_map.cc - provides an unordered_map-like data structure with
// fine-grained locking.
//
// TODO(amb): a singly-linked, intrusive list would be a useful cleanup.

#include "hogd/ramdb/scalable_map.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/log_allocator.h"

namespace hogd {

namespace {

inline OwningSpan<const std::byte>
make_byte_buffer(std::span<const std::byte> value)
{
  auto buf = std::make_unique_for_overwrite<std::byte[]>(value.size());
  std::ranges::copy(value, buf.get());
  auto *base_ptr = buf.get();
  return OwningSpan(std::span<const std::byte>(base_ptr, value.size()),
                    [ref = std::move(buf)] {});
}

void
counter_inc(std::atomic<uint16_t> &cnt)
{
  cnt.store(cnt.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void
counter_dec(std::atomic<uint16_t> &cnt)
{
  cnt.store(cnt.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
}

} // namespace

ATTRIBUTE_ALWAYS_INLINE std::tuple<std::unique_lock<std::mutex>, ItemHeader **,
                                   std::atomic<uint16_t> &>
ScalableMap::acquire_bucket(std::span<const std::byte> key)
{
  assert(std::has_single_bit(mutexes_.size()));
  assert(std::has_single_bit(buckets_.size()));
  assert(std::has_single_bit(old_size_));

  // Calculate the hash.
  size_t hash = std::hash<std::span<const std::byte>>{}(key);

  // Then determine which lock to acquire.
  std::mutex &mu = mutexes_[hash & (mutexes_.size() - 1)];
  std::unique_lock<std::mutex> ul(mu);

  // Check if the bucket hasn't yet been expanded.
  size_t idx = hash & (old_size_ - 1);
  if (idx >= expanded_idx_.load(std::memory_order_relaxed))
    return {std::move(ul), &buckets_[idx], item_counts_[idx]};

  // Otherwise return the normal bucket.
  idx = hash & (buckets_.size() - 1);
  return {std::move(ul), &buckets_[idx], item_counts_[idx]};
}

inline void
ScalableMap::unlink(ItemHeader **item_ptr)
{
  ItemHeader *item = *item_ptr;
  *item_ptr = item->next;
  allocator_.deallocate(item);
}

DBStatus<DBItem>
ScalableMap::get(std::span<const std::byte> key)
{
  auto [ul, bu, ignore] = acquire_bucket(key);
  for (; *bu != nullptr; bu = &(*bu)->next) {
    if ((*bu)->get_key() == key) {
      return DBItem{.type = (*bu)->type,
                    .value = make_byte_buffer((*bu)->get_value())};
    }
  }
  return DBError(DBCode::kNotFound);
}

DBStatus<void>
ScalableMap::get_if_type(std::span<const std::byte> key, uint8_t type,
                         std::span<std::byte> val_out)
{
  auto [ul, bu, ignore] = acquire_bucket(key);
  for (; *bu != nullptr; bu = &(*bu)->next) {
    if ((*bu)->get_key() == key) {
      if ((*bu)->type != type || (*bu)->get_value().size() != val_out.size())
        return DBError(DBCode::kTypeMismatch);

      // success, copy out the item's value.
      std::ranges::copy((*bu)->get_value(), val_out.begin());
      return {};
    }
  }
  return DBError(DBCode::kNotFound);
}

DBStatus<void>
ScalableMap::put(std::span<const std::byte> key, uint8_t type,
                 std::span<const std::byte> val, bool replace)
{
  if (key.size() > std::numeric_limits<uint16_t>::max() ||
      val.size() > std::numeric_limits<uint32_t>::max()) [[unlikely]] {
    return DBError(DBCode::kTooBig);
  }

  // allocate and initialize the item.
  // WARNING: calling allocate with the bucket lock held could deadlock since
  // the compactor also could use this lock.
  ItemHeader *item = allocator_.allocate(type, key.size(), val.size());
  if (!item) [[unlikely]]
    return DBError(DBCode::kNoMemory);
  std::ranges::copy(key, item->get_key().begin());
  std::ranges::copy(val, item->get_value().begin());

  auto [ul, bu, cnt] = acquire_bucket(key);
  ItemHeader **head = bu;

  // first check if the key already exists.
  for (; *bu != nullptr; bu = &(*bu)->next) {
    if ((*bu)->get_key() == key) {
      if (!replace) {
        allocator_.deallocate(item);
        return DBError(DBCode::kExists);
      }

      unlink(bu);
      counter_dec(cnt);
      break;
    }
  }

  // then insert key-value pair.
  item->next = *head;
  *head = item;
  counter_inc(cnt);
  return {};
}

DBStatus<void>
ScalableMap::remove(std::span<const std::byte> key)
{
  auto [ul, bu, cnt] = acquire_bucket(key);
  for (; *bu != nullptr; bu = &(*bu)->next) {
    if ((*bu)->get_key() == key) {
      unlink(bu);
      counter_dec(cnt);
      break;
    }
  }
  return {};
}

bool
ScalableMap::prepare_expand(int bucket_order)
{
  size_t new_size = 1UL << bucket_order;

  // we don't support shrinking.
  if (new_size <= buckets_.size())
    return false;

  info("expanding map from {} to {} buckets", buckets_.size(), new_size);

  // make the item count table larger.
  std::vector<std::atomic<uint16_t>> new_item_counts(new_size);
  for (size_t i = 0; i < item_counts_.size(); ++i) {
    new_item_counts[i] = item_counts_[i].load(std::memory_order_relaxed);
  }
  item_counts_ = std::move(new_item_counts);

  // make the bucket table larger.
  old_size_ = buckets_.size();
  buckets_.resize(new_size);

  // setup resizing.
  expanded_idx_.store(0, std::memory_order_relaxed);
  return true;
}

void
ScalableMap::do_expand()
{
  using clock = std::chrono::high_resolution_clock;
  clock::time_point start = clock::now();

  for (size_t idx = 0; idx < old_size_; ++idx) {
    std::mutex &mu = mutexes_[idx & (mutexes_.size() - 1)];
    std::lock_guard guard(mu);
    for (ItemHeader **bu = &buckets_[idx]; *bu != nullptr;) {
      size_t hash = std::hash<std::span<const std::byte>>{}((*bu)->get_key());
      size_t new_idx = hash & (buckets_.size() - 1);
      ItemHeader **new_bu = &buckets_[new_idx];

      // continue if the item's bucket didn't change.
      if (new_bu == bu) {
        bu = &(*bu)->next;
        continue;
      }

      // otherwise move the item to the new bucket.
      ItemHeader *item = *bu;
      *bu = item->next;
      item->next = *new_bu;
      *new_bu = item;
      counter_dec(item_counts_[idx]);
      counter_inc(item_counts_[new_idx]);

      // increment index of expanded buckets.
      expanded_idx_.store(idx + 1, std::memory_order_relaxed);
    }
  }

  clock::time_point end = clock::now();
  auto seconds = std::chrono::duration<double>(end - start).count();
  info("finished expanding in {:.3f} seconds", seconds);
}

bool
ScalableMap::modify_locked(std::span<const std::byte> key,
                           const std::function<void(ItemHeader **h)> &fn)
{
  auto [ul, bu, ignore] = acquire_bucket(key);
  for (; *bu != nullptr; bu = &(*bu)->next) {
    if ((*bu)->get_key() == key) {
      fn(bu);
      return true;
    }
  }
  return false;
}

} // namespace hogd
