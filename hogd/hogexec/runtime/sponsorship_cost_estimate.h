#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/item/item_view.h"
#include "hogd/hogexec/runtime/item/item_view_type.h"
#include "hogd/hogexec/runtime/item/snapshot_storage_item.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "storage_item.hh"

namespace hogd {

// Describes a storage write for cost estimation purposes.
struct StorageWriteInfo {
  // What type of write is being performed.
  enum class WriteType {
    kBytestring,
    kNNInt,
    kDelete,
    kDeployContract,
    // Note: auth data, RCQ, replay cache, and balance writes are NOT
    // sponsorable in the current design (they are system-level writes,
    // not user contract data writes).
  };

  WriteType type;

  // For bytestring writes: the length of the new bytestring.
  size_t bytestring_len = 0;

  // For deploy_contract: the contract size in bytes.
  uint32_t contract_size = 0;

  // The flags on this write (used to determine exclusive vs non-exclusive).
  StorageDeltaFlagSet flags = 0;
};

// Compute the prior storage cost of an item from a SnapshotStorageItem.
//
// Returns the cost of the item as it exists in the snapshot. If the item
// does not exist (has no type), returns 0.
inline int128_t compute_snapshot_prior_cost(
    const SnapshotStorageItem& snapshot,
    const StorageCostModel& cost_model)
{
  auto item_type = snapshot.type();
  if (!item_type.has_value()) {
    // Item does not exist in the snapshot.
    return 0;
  }

  switch (*item_type) {
  case ItemViewType::kBytestring:
    return cost_model.get_bytestring_cost(snapshot.bytestring().size());
  case ItemViewType::kNNInt:
  case ItemViewType::kNativeTokenBalance:
    return cost_model.get_nnint_cost();
  case ItemViewType::kRetrocausalQueue:
    return cost_model.get_retrocausal_queue_cost(
        static_cast<int64_t>(
            snapshot.retrocausal_queue_metadata().live_entries));
  case ItemViewType::kReplayCache:
    // ReplayCacheEntry in snapshot doesn't carry size information.
    // Use kBaseItemCost as a conservative estimate.
    return cost_model.get_replay_cache_cost(0);
  case ItemViewType::kAddressAuthData:
    // Conservative: we don't have easy access to key count from the
    // snapshot entry type, so use base item cost.
    return cost_model.kBaseItemCost;
  }
  // Unknown type: conservative fallback.
  return 0;
}

// Compute the prior storage cost from an ItemView's current state.
//
// This reflects the cost of the item as of the start of this write:
// - If the item was read from DB (UnmodifiedItem with snapshot): snapshot cost
// - If the item was written earlier in the tx: cost of the previous write
// - If the item is new / unloaded: 0
//
// Takes a non-const ItemView& because ItemView::get<T>() is non-const.
inline int128_t compute_prior_cost(
    ItemView& item_view,
    const StorageCostModel& cost_model)
{
  // Probe each variant type in order.

  if (auto* unmod = item_view.get<UnmodifiedItem>()) {
    if (!unmod->is_loaded()) {
      // Snapshot not loaded: blind write, assume new item.
      return 0;
    }
    return compute_snapshot_prior_cost(unmod->base_item(), cost_model);
  }

  if (const auto* bs = item_view.get<BytestringItem>()) {
    return cost_model.get_bytestring_cost(bs->bytestring_.size());
  }

  if (item_view.get<NNIntItem>()) {
    return cost_model.get_nnint_cost();
  }

  if (item_view.get<NativeTokenBalanceItem>()) {
    return cost_model.get_nnint_cost();
  }

  if (item_view.get<DeletedItem>()) {
    // Item was deleted: effectively no prior cost.
    return 0;
  }

  // RetrocausalQueueItem, ReplayCacheItem, AddressAuthDataItem:
  // These are system-level writes that are not sponsorable. Callers
  // should not be computing marginal cost for them. Return 0 so that
  // accidental inclusion doesn't charge the sponsor.
  if (item_view.get<RetrocausalQueueItem>()) {
    return 0;
  }

  if (item_view.get<ReplayCacheItem>()) {
    return 0;
  }

  if (item_view.get<AddressAuthDataItem>()) {
    return 0;
  }

  // Should not reach here: all variant alternatives are covered.
  assert(false && "unreachable: unknown ItemView variant");
  return 0;
}

// Compute the new storage cost for a write operation.
inline int128_t compute_new_cost(
    const StorageWriteInfo& write_info,
    const StorageCostModel& cost_model)
{
  switch (write_info.type) {
  case StorageWriteInfo::WriteType::kBytestring:
    return cost_model.get_bytestring_cost(write_info.bytestring_len);
  case StorageWriteInfo::WriteType::kNNInt:
    return cost_model.get_nnint_cost();
  case StorageWriteInfo::WriteType::kDelete:
    return 0;
  case StorageWriteInfo::WriteType::kDeployContract:
    return cost_model.kBaseContractDeploymentCost
         + cost_model.get_contract_size_cost(write_info.contract_size);
  }
  // Unknown write type: should not happen.
  assert(false && "unreachable: unknown WriteType");
  return 0;
}

// Compute the marginal cost delta between new and prior item cost.
// Mirrors the DB's snapshot_cost_transform logic in item_lock.cc:
//   - For exclusive writes: new_cost - prior_cost (can be negative)
//   - For non-exclusive writes: max(new_cost - prior_cost, 0)
//
// Note: the DB version uses `is_permanent_exclusive` which maps to the
// executor's kStorageDeltaFlagExclusiveLock flag via storage_view.cc.
inline int128_t marginal_cost_delta(
    int128_t new_cost,
    int128_t prior_cost,
    bool is_exclusive)
{
  if (is_exclusive) {
    return new_cost - prior_cost;
  }
  return std::max<int128_t>(new_cost - prior_cost, 0);
}

// Estimate the marginal storage cost of a write operation.
//
// This mirrors the DB's snapshot_cost_transform logic:
// - For exclusive writes: cost(new) - cost(old), can be negative
// - For non-exclusive writes: max(cost(new) - cost(old), 0)
//
// Parameters:
// - item_view: the current state of the item (from the frame stack).
//              Non-const because ItemView::get<T>() requires it.
// - write_info: describes the write being performed
// - cost_model: the storage cost model
//
// Returns the estimated marginal storage cost.
inline int128_t estimate_marginal_storage_cost(
    ItemView& item_view,
    const StorageWriteInfo& write_info,
    const StorageCostModel& cost_model)
{
  const int128_t new_cost = compute_new_cost(write_info, cost_model);
  const int128_t prior_cost = compute_prior_cost(item_view, cost_model);

  const bool is_exclusive =
      (write_info.flags &
       std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagExclusiveLock))
      != 0;

  return marginal_cost_delta(new_cost, prior_cost, is_exclusive);
}

} // namespace hogd
