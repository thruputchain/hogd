// scalable_map.cc - provides an unordered_map-like data structure with
// fine-grained locking.
//
// TODO(amb): a singly-linked, intrusive list would be a useful cleanup.

#include "hogd/ramdb/scalable_map.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
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
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/ramdb/constants.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/disk_writer.h"
#include "hogd/ramdb/item_allocator.h"
#include "hogd/ramdb/loglet.h"

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

inline void
counter_inc(std::atomic<uint16_t> &cnt)
{
  cnt.store(cnt.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

inline void
counter_dec(std::atomic<uint16_t> &cnt)
{
  cnt.store(cnt.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
}

inline size_t
calculate_hash(std::span<const std::byte> key)
{
  return SipHash24::get_default()(key);
}

} // namespace

ATTRIBUTE_ALWAYS_INLINE std::tuple<std::unique_lock<std::mutex>, ItemHeader **,
                                   std::atomic<uint16_t> &>
ScalableMap::acquire_bucket(size_t hash)
{
  assert(std::has_single_bit(mutexes_.size()));
  assert(std::has_single_bit(buckets_.size()));
  assert(std::has_single_bit(old_size_));

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
  allocator_.free(item);
}

DBStatus<DBItem>
ScalableMap::get(std::span<const std::byte> key)
{
  auto [ul, bu, ignore] = acquire_bucket(calculate_hash(key));
  for (; *bu != nullptr; bu = &(*bu)->next) {
    if ((*bu)->get_key() == key) {
      return DBItem{.type = (*bu)->type,
                    .value = make_byte_buffer((*bu)->get_value())};
    }
  }
  return DBError(DBCode::kNotFound);
}

DBStatus<OwningSpan<const std::byte>>
ScalableMap::get_if_type(std::span<const std::byte> key, uint8_t type)
{
  auto [ul, bu, ignore] = acquire_bucket(calculate_hash(key));
  for (; *bu != nullptr; bu = &(*bu)->next) {
    if ((*bu)->get_key() == key) {
      if ((*bu)->type != type)
        return DBError(DBCode::kTypeMismatch);
      return make_byte_buffer((*bu)->get_value());
    }
  }
  return DBError(DBCode::kNotFound);
}

DBStatus<void>
ScalableMap::get_if_type(std::span<const std::byte> key, uint8_t type,
                         std::span<std::byte> val_out)
{
  auto [ul, bu, ignore] = acquire_bucket(calculate_hash(key));
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

DBStatus<size_t>
ScalableMap::get_size(std::span<const std::byte> key)
{
  auto [ul, bu, ignore] = acquire_bucket(calculate_hash(key));
  for (; *bu != nullptr; bu = &(*bu)->next) {
    if ((*bu)->get_key() != key)
      continue;

    return (*bu)->get_value().size();
  }
  return DBError(DBCode::kNotFound);
}

DBStatus<void>
ScalableMap::put(std::span<const std::byte> key, uint8_t type,
                 std::span<const std::byte> val)
{
  if (key.size() > std::numeric_limits<uint16_t>::max() ||
      val.size() > std::numeric_limits<uint32_t>::max() ||
      sizeof(ItemHeader) + key.size() + val.size() > kBigItemThreshold)
      [[unlikely]] {
    return DBError(DBCode::kTooBig);
  }

  // allocate and initialize the item.
  // WARNING: calling alloc() with the bucket lock held could deadlock since
  // the compactor may also acquire this lock.
  ItemHeader *item = allocator_.alloc(type, key.size(), val.size());
  if (!item) [[unlikely]]
    return DBError(DBCode::kNoMemory);
  std::ranges::copy(key, item->get_key().begin());
  std::ranges::copy(val, item->get_value().begin());

  // Write to on-disk log first.
  size_t hash = calculate_hash(key);
  ScopedDiskWrite record = disk_writer_.write_record(hash);
  DBStatus<uint64_t> ret = record.modify(item);
  if (!ret) [[unlikely]]
    return ret.error();

  const uint64_t new_lsn = *ret;
  item->lsn = new_lsn;

  auto [ul, bu, cnt] = acquire_bucket(hash);
  record.reset();
  ItemHeader **head = bu;

  // Check if the key already exists.
  for (; *bu != nullptr; bu = &(*bu)->next) {
    if ((*bu)->get_key() == key) {
      // If the item that is about to be overwritten should be included in an
      // ongoing snapshot, make sure the snapshot gets a copy of the item.
      // This must be done under the bucket lock because we must ensure the
      // snapshotter doesn't copy an old value for this key into the snapshot.
      if (mutation_cb_)
        mutation_cb_(*bu);

      // Is the item size unchanged? Update in place.
      if ((*bu)->get_value().size() == val.size()) {
        (*bu)->type = type;
        // Updating the LSN for update-in-place ensures that we don't
        // inadvertently forward this new value into an ongoing snapshot.
        (*bu)->lsn = new_lsn;
        std::ranges::copy(val, (*bu)->get_value().begin());
        allocator_.free<true>(item);
        return {};
      }

      // Otherwise delete so it can be inserted as new.
      unlink(bu);
      counter_dec(cnt);
      break;
    }
  }

  // Insert new key-value pair.
  item->next = *head;
  *head = item;
  counter_inc(cnt);
  return {};
}

DBStatus<void>
ScalableMap::remove(std::span<const std::byte> key)
{
  // Write to the on-disk log first.
  size_t hash = calculate_hash(key);
  ScopedDiskWrite record = disk_writer_.write_record(hash);
  DBStatus<void> ret = record.remove(key);
  if (!ret) [[unlikely]]
    return ret.error();

  // Then remove the in-memory item.
  auto [ul, bu, cnt] = acquire_bucket(hash);
  record.reset();
  for (; *bu != nullptr; bu = &(*bu)->next) {
    if ((*bu)->get_key() == key) {
      // See comment in put(). Forwards the item to an ongoing snapshot
      // that should include it before removing it.
      if (mutation_cb_)
        mutation_cb_(*bu);

      unlink(bu);
      counter_dec(cnt);
      break;
    }
  }
  return {};
}

DBStatus<void>
ScalableMap::apply_change(ItemHeader *hdr)
{
  size_t idx = calculate_hash(hdr->get_key()) & (buckets_.size() - 1);
  ItemHeader **bu = &buckets_[idx];
  std::atomic<uint16_t> &cnt = item_counts_[idx];

  using enum ItemStorage;
  assert(hdr->storage == kNormal || hdr->storage == kTombstone);
  switch (hdr->storage) {

  case kNormal: // handle put
  {
    // Check if the key already exists.
    for (; *bu != nullptr; bu = &(*bu)->next) {
      if ((*bu)->get_key() == hdr->get_key()) {
        // Is the item size unchanged? Update in place.
        if ((*bu)->get_value().size() == hdr->get_value().size()) {
          (*bu)->type = hdr->type;
          (*bu)->lsn = hdr->lsn;
          std::ranges::copy(hdr->get_value(), (*bu)->get_value().begin());
          return {};
        }

        // Otherwise delete so it can be inserted as new.
        unlink(bu);
        counter_dec(cnt);
        break;
      }
    }

    // Insert new key-value pair.
    ItemHeader *item = allocator_.alloc(hdr->type, hdr->get_key().size(),
                                        hdr->get_value().size());
    if (!item) [[unlikely]]
      return DBError(DBCode::kNoMemory);
    std::ranges::copy(hdr->get_key_and_value(),
                      item->get_key_and_value().begin());
    item->lsn = hdr->lsn;
    item->next = *bu;
    *bu = item;
    counter_inc(cnt);
    break;
  }

  case kTombstone: // handle remove
  {
    for (; *bu != nullptr; bu = &(*bu)->next) {
      if ((*bu)->get_key() == hdr->get_key()) {
        unlink(bu);
        counter_dec(cnt);
        break;
      }
    }
    break;
  }

  default:
    std::unreachable();
  }

  return {};
}

bool
ScalableMap::prepare_expand()
{
  // calculate ideal size.
  int new_bucket_order = std::countr_zero(std::bit_floor(size()));
  size_t new_size = 1UL << new_bucket_order;

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
  ElapsedTimer time;

  for (size_t idx = 0; idx < old_size_; ++idx) {
    std::mutex &mu = mutexes_[idx & (mutexes_.size() - 1)];
    std::lock_guard guard(mu);
    for (ItemHeader **bu = &buckets_[idx]; *bu != nullptr;) {
      size_t new_idx = calculate_hash((*bu)->get_key()) & (buckets_.size() - 1);
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
    }

    // increment expanded_idx_ to mark this bucket as fully expanded.
    expanded_idx_.store(idx + 1, std::memory_order_relaxed);
  }

  assert(expanded_idx_.load() == old_size_);
  info("finished expanding in {:.3f} seconds", time.elapsed());
}

bool
ScalableMap::modify_locked(
    std::span<const std::byte> key,
    const std::function<void(ItemHeader **h, size_t hash)> &fn)
{
  size_t hash = calculate_hash(key);
  auto [ul, bu, ignore] = acquire_bucket(hash);
  for (; *bu != nullptr; bu = &(*bu)->next) {
    if ((*bu)->get_key() == key) {
      fn(bu, hash);
      return true;
    }
  }
  return false;
}

void
ScalableMap::register_mutation_cb(std::function<void(ItemHeader *)> cb)
{
  std::vector<std::unique_lock<std::mutex>> locks{mutexes_.begin(),
                                                  mutexes_.end()};
  mutation_cb_ = std::move(cb);
}

} // namespace hogd
