#include "hogd/hogexec/runtime/snapshot_storage_item.h"

#include <cassert>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/status.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

SnapshotStorageItem::variant_type
SnapshotStorageItem::make_contract_key_variant(StorageItem item)
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
    return variant_type{std::in_place_type<RetrocausalQueueEntry>};
  case StorageItemType::kItemTypeDeleted:
    return variant_type{std::in_place_type<DeletedEntry>};
  }
  assert(false);
  std::unreachable();
}

SnapshotStorageItem::variant_type
SnapshotStorageItem::make_variant(StorageItem item, StorageKey const &db_key)
{
  // handle different key types differently
  switch (db_key.type()) {
  case StorageKeyType::kStorageKeyTypeContractData:
    return make_contract_key_variant(item);
  case StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance:
    switch (item.type()) {
    case StorageItemType::kItemTypeNonnegativeInt:
      return variant_type{std::in_place_type<NativeTokenBalanceEntry>,
                      from_xdr(item.nnint())};
    case StorageItemType::kItemTypeDeleted:
      return variant_type{std::in_place_type<NativeTokenBalanceEntry>,
                      0};
    default:
      assert(false);
      std::unreachable();
    }
  default:
    assert(false);
    std::unreachable();
  }
}

SystemStatus<const RetrocausalQueueBlock *>
SnapshotStorageItem::RetrocausalQueueEntry::get_next_block(
    DatabaseProxy &db_proxy, BlockID block_id, AddressAndKey const &addrkey,
    uint64_t search_start_index)
{
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
                                                          search_start_index)();
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
SnapshotStorageItem::get_next_rcq_block(uint64_t search_start_block)
{
  // This method is only valid for RCQ items
  if (!std::holds_alternative<RetrocausalQueueEntry>(entry_)) {
    return nullptr;
  }
  auto &rcq = std::get<RetrocausalQueueEntry>(entry_);
  std::lock_guard<std::mutex> lock(rcq_mtx_);
  return rcq.get_next_block(db_proxy_, block_id_, addrkey_, search_start_block);
}

SystemStatus<Nondeterministic<std::optional<QueueEntry>>>
SnapshotStorageItem::get_concurrent_rcq_entry(uint32_t index)
{
  std::lock_guard<std::mutex> lock(rcq_mtx_);
  if (index < concurrent_queue_entries_.size()) {
    return concurrent_queue_entries_[index];
  }

  auto db_res =
      db_proxy_.get_concurrent_retrocausal_queue_entry(block_id_, addrkey_,
                                                       index)();
  if (!db_res) {
    // the type deduction gets messed up without the explicit std::unexpected
    return std::unexpected{db_res.error()};
  }
  auto entry = db_res.value();
  if (entry.has_value()) {
    concurrent_queue_entries_.push_back(*entry);
  }
  return entry;
}

} // namespace hogd
