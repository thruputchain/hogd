#include "hogd/hogexec/runtime/item/snapshot_storage_item.h"

#include <cassert>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

SnapshotStorageItem::variant_type
SnapshotStorageItem::make_contract_key_variant(WireStorageItem item)
{
  switch (item.type()) {
  case StorageItemType::kItemTypeBytestring:
    return variant_type{std::in_place_type<BytestringEntry>,
                        std::move(item.bytestring())};
  case StorageItemType::kItemTypeNonnegativeInt:
  {
    return variant_type{std::in_place_type<NNIntEntry>, from_xdr(item.nnint())};
  }
  case StorageItemType::kItemTypeRetrocausalQueue:
    return variant_type{std::in_place_type<RetrocausalQueueEntry>,
                        std::move(item.metadata())};
  case StorageItemType::kItemTypeDeleted:
    return variant_type{std::in_place_type<DeletedEntry>};
  case StorageItemType::kItemTypeReplayCacheData:
    return variant_type{std::in_place_type<ReplayCacheEntry>};
  case StorageItemType::kItemTypeAuthData:
    throw std::runtime_error("Auth data not yet supported");
  case StorageItemType::kItemTypeDeployedContractHash:
    throw std::runtime_error("Deployed contract hash cached elsewhere");
  }
  assert(false);
  throw std::runtime_error("Impossible");
}

SnapshotStorageItem::variant_type
SnapshotStorageItem::make_variant(WireStorageItem item,
                                  StorageKey const &db_key)
{
  // handle different key types differently
  switch (db_key.key_type()) {
  case StorageKeyType::kStorageKeyTypeContractData:
    return make_contract_key_variant(item);
  case StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance:
    switch (item.type()) {
    case StorageItemType::kItemTypeNonnegativeInt:
      return variant_type{std::in_place_type<NativeTokenBalanceEntry>,
                          from_xdr(item.nnint())};
    case StorageItemType::kItemTypeDeleted:
      return variant_type{std::in_place_type<NativeTokenBalanceEntry>, 0};
    case StorageItemType::kItemTypeAuthData:
      throw std::runtime_error("Auth data not yet supported");
    case StorageItemType::kItemTypeReplayCacheData:
      throw std::runtime_error("Replay cache data not yet supported");
    default:
      assert(false);
      throw std::runtime_error("Impossible");
    }
  default:
    assert(false);
    throw std::runtime_error("Impossible");
  }
}

SystemStatus<const RetrocausalQueueBlock *>
SnapshotStorageItem::RetrocausalQueueEntry::get_next_block(
    DatabaseProxy &db_proxy, BlockID block_id, AddressAndKey const &addrkey,
    uint64_t search_start_index, std::unique_lock<mutex_type> &ul) const
{
  assert(ul.owns_lock());
  // If we've already queried for a block below search_start_index and that
  // search failed, then there's no reason to query the database again.
  if (block_number_upper_bound_ &&
      search_start_index >= *block_number_upper_bound_) {
    return nullptr;
  }

  auto it = blocks_.lower_bound(search_start_index);
  if (it != blocks_.end()) {
    return it->second.get();
  }

  auto db_res = db_proxy.get_next_retrocausal_queue_block(block_id, addrkey,
                                                          search_start_index);
  if (!db_res) {
    return db_res.error();
  }
  auto block = db_res.value();

  if (!block) {
    // cache that there isn't a block above search_start_index
    assert(!block_number_upper_bound_ ||
           search_start_index < *block_number_upper_bound_);
    block_number_upper_bound_ = search_start_index;
    return nullptr;
  }

  auto ptr = std::make_unique<RetrocausalQueueBlock>(*block);
  auto const *out = ptr.get();
  blocks_[block->block_number] = std::move(ptr);
  return out;
}

SystemStatus<const RetrocausalQueueBlock *>
SnapshotStorageItem::get_next_rcq_block(uint64_t search_start_block) const
{
  // This method is only valid for RCQ items
  if (!std::holds_alternative<RetrocausalQueueEntry>(entry_)) {
    return nullptr;
  }
  auto const &rcq = std::get<RetrocausalQueueEntry>(entry_);
  std::unique_lock ul(rcq_mtx_);
  return rcq.get_next_block(db_proxy_, block_id_, addrkey_, search_start_block,
                            ul);
}

SystemStatus<Nondeterministic<std::optional<QueueEntry>>>
SnapshotStorageItem::get_concurrent_rcq_entry(uint32_t index) const
{
  {
    std::lock_guard ul(rcq_mtx_);
    if (index < concurrent_queue_entries_.size()) {
      return concurrent_queue_entries_[index];
    }
    if (index != concurrent_queue_entries_.size()) [[unlikely]] {
      error("unrecoverable: bad get_concurrent_rcq_entry access");
      return SystemError{SystemErrorCode::kSystemUnrecoverable};
    }
  }

  auto db_res =
      db_proxy_.get_concurrent_retrocausal_queue_entry(block_id_, addrkey_,
                                                       index);
  if (!db_res) {
    return db_res.error();
  }
  auto entry = db_res.value();
  if (entry.has_value()) {
    std::lock_guard ul(rcq_mtx_);
    if (index == concurrent_queue_entries_.size()) {
      concurrent_queue_entries_.push_back(*entry);
    }
    else if (concurrent_queue_entries_[index] != *entry) [[unlikely]] {
      error("unrecoverable: mismatch in concurrent rcq return results");
      return SystemError{SystemErrorCode::kSystemUnrecoverable};
    }
  }
  return entry;
}

int128_t
SnapshotStorageItem::snapshot_storage_cost(StorageCostModel const &model) const
{
  return std::visit(
      [&](auto const &arg) -> int128_t {
        return arg.snapshot_storage_cost(model);
      },
      entry_);
}

int128_t
SnapshotStorageItem::BytestringEntry::snapshot_storage_cost(
    StorageCostModel const &model) const
{
  return model.get_bytestring_cost(bytestring_.size());
}

int128_t
SnapshotStorageItem::NNIntEntry::snapshot_storage_cost(
    StorageCostModel const &model)
{
  return model.get_nnint_cost();
}

int128_t
SnapshotStorageItem::ReplayCacheEntry::snapshot_storage_cost(
    StorageCostModel const & /*unused*/)
{
  // We never actually access the replay cache snapshot cost -- the replay cache
  // modifications can never be sponsored (as they're only ever applied in the
  // root frame). We may, in the future, want to add something to the
  // transaction format to allow another account to sponsor from the root frame.
  // At such a time, we would need to address this assumption.
  //
  // But that also opens up other questions, such as what to do about the case
  // of the base item cost for the replay cache. Do we want to add a bespoke
  // flag on that hypothetical transaction format to distinguish whether the
  // base item exists already or not?  Maybe the simplest option would be to get
  // rid of the base item cost on something like the replay cache, as we're
  // expecting every address that originates transactions to have one anyways?
  // Those questions are deferred until a later date.
  panic("We should never be accessing the replay cache storage cost");
}

int128_t
SnapshotStorageItem::NativeTokenBalanceEntry::snapshot_storage_cost(
    StorageCostModel const &model)
{
  return model.get_balance_cost();
}

int128_t
SnapshotStorageItem::AddressAuthDataEntry::snapshot_storage_cost(
    StorageCostModel const &model) const
{
  size_t active_key_count = 0;
  for (auto const &pk : address_auth_data_.public_keys) {
    if (pk) {
      active_key_count++;
    }
  }
  return model.get_auth_data_cost(active_key_count);
}

int128_t
SnapshotStorageItem::RetrocausalQueueEntry::snapshot_storage_cost(
    StorageCostModel const &model) const
{
  return model.get_retrocausal_queue_cost(metadata_.live_entries);
}

} // namespace hogd
