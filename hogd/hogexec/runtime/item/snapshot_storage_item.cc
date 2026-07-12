#include "hogd/hogexec/runtime/item/snapshot_storage_item.h"

#include <cassert>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

SystemStatus<void>
SnapshotStorageItem::validate_wire_item(StorageKey const &db_key,
                                        WireStorageItem const &item)
{
  switch (db_key.key_type()) {
  case StorageKeyType::ContractData:
    switch (item.type()) {
    case StorageItemType::kItemTypeBytestring:
    case StorageItemType::kItemTypeNonnegativeInt:
    case StorageItemType::kItemTypeRetrocausalQueue:
    case StorageItemType::kItemTypeDeleted:
    case StorageItemType::kItemTypeReplayCacheData:
      return {};
    default:
      error("Unrecoverable: unsupported wire item type {} for ContractData key",
            static_cast<int>(item.type()));
      return SystemError{SystemErrorCode::Unrecoverable};
    }
  case StorageKeyType::FreeNativeTokenBalance:
    switch (item.type()) {
    case StorageItemType::kItemTypeNonnegativeInt:
    case StorageItemType::kItemTypeDeleted:
      return {};
    default:
      error("Unrecoverable: unsupported wire item type {} for "
            "FreeNativeTokenBalance key",
            static_cast<int>(item.type()));
      return SystemError{SystemErrorCode::Unrecoverable};
    }
  case StorageKeyType::AuthData:
    switch (item.type()) {
    case StorageItemType::kItemTypeAuthData:
    case StorageItemType::kItemTypeDeleted:
      return {};
    default:
      error("Unrecoverable: unsupported wire item type {} for AuthData key",
            static_cast<int>(item.type()));
      return SystemError{SystemErrorCode::Unrecoverable};
    }
  default:
    error("Unrecoverable: unsupported storage key type {}",
          static_cast<int>(db_key.key_type()));
    return SystemError{SystemErrorCode::Unrecoverable};
  }
}

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
    panic("Auth data not supported for contract-data key (should be gated by "
          "validate_wire_item)");
  case StorageItemType::kItemTypeDeployedContractHash:
    panic("Deployed contract hash cached elsewhere (should be gated by "
          "validate_wire_item)");
  }
  panic("Impossible: unhandled StorageItemType in make_contract_key_variant");
}

SnapshotStorageItem::variant_type
SnapshotStorageItem::make_variant(WireStorageItem item,
                                  StorageKey const &db_key)
{
  // handle different key types differently
  switch (db_key.key_type()) {
  case StorageKeyType::ContractData:
    return make_contract_key_variant(item);
  case StorageKeyType::FreeNativeTokenBalance:
    switch (item.type()) {
    case StorageItemType::kItemTypeNonnegativeInt:
      return variant_type{std::in_place_type<NativeTokenBalanceEntry>,
                          from_xdr(item.nnint())};
    case StorageItemType::kItemTypeDeleted:
      return variant_type{std::in_place_type<NativeTokenBalanceEntry>, 0};
    case StorageItemType::kItemTypeAuthData:
      panic("Auth data not supported for native-token-balance key (should be "
            "gated by validate_wire_item)");
    case StorageItemType::kItemTypeReplayCacheData:
      panic("Replay cache data not supported for native-token-balance key "
            "(should be gated by validate_wire_item)");
    default:
      panic("Impossible: unhandled item type for native-token-balance key");
    }
  case StorageKeyType::AuthData:
    switch (item.type()) {
    case StorageItemType::kItemTypeAuthData:
      return variant_type{std::in_place_type<AddressAuthDataEntry>,
                          std::move(item.auth_data())};
    case StorageItemType::kItemTypeDeleted:
      return variant_type{std::in_place_type<DeletedEntry>};
    default:
      panic("Impossible: unhandled item type for auth-data key");
    }
  default:
    panic("Impossible: unhandled StorageKeyType in make_variant");
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
      return SystemError{SystemErrorCode::Unrecoverable};
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
      return SystemError{SystemErrorCode::Unrecoverable};
    }
  }
  return entry;
}

} // namespace hogd
