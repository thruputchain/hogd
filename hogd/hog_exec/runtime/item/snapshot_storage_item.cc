#include "hogd/hog_exec/runtime/item/snapshot_storage_item.h"

#include <cassert>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"

namespace hogd {

SnapshotStorageItem::SnapshotStorageItem(variant_type entry,
                                         AddressAndKey addrkey,
                                         BlockID block_id,
                                         DatabaseProxy &db_proxy)
  : addrkey_(std::move(addrkey)), entry_(std::move(entry)), block_id_(block_id),
    db_proxy_(db_proxy)
{
  assert(&db_proxy_);
}

SystemStatus<SnapshotStorageItem::variant_type>
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
  default:
    error("Unrecoverable: unsupported wire item type {} for ContractData key",
          static_cast<int>(item.type()));
    return SystemError{SystemErrorCode::Unrecoverable};
  }
}

SystemStatus<SnapshotStorageItem::variant_type>
SnapshotStorageItem::make_variant(WireStorageItem item,
                                  StorageKey const &db_key)
{
  // handle different key types differently
  switch (db_key.key_type()) {
  case StorageKeyType::ContractData:
    return make_contract_key_variant(std::move(item));
  case StorageKeyType::FreeNativeTokenBalance:
    switch (item.type()) {
    case StorageItemType::kItemTypeNonnegativeInt:
      return variant_type{std::in_place_type<NativeTokenBalanceEntry>,
                          from_xdr(item.nnint())};
    case StorageItemType::kItemTypeDeleted:
      return variant_type{std::in_place_type<NativeTokenBalanceEntry>, 0};
    default:
      error("Unrecoverable: unsupported wire item type {} for "
            "FreeNativeTokenBalance key",
            static_cast<int>(item.type()));
      return SystemError{SystemErrorCode::Unrecoverable};
    }
  case StorageKeyType::AuthData:
    switch (item.type()) {
    case StorageItemType::kItemTypeAuthData:
      return variant_type{std::in_place_type<AddressAuthDataEntry>,
                          std::move(item.auth_data())};
    case StorageItemType::kItemTypeDeleted:
      return variant_type{std::in_place_type<DeletedEntry>};
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

x23::Async<SystemStatus<const RetrocausalQueueBlock *>>
SnapshotStorageItem::RetrocausalQueueEntry::get_next_block(
    DatabaseProxy &db_proxy, BlockID block_id, AddressAndKey const &addrkey,
    uint64_t search_start_index) const
{
  // If we've already queried for a block below search_start_index and that
  // search failed, then there's no reason to query the database again.
  if (block_number_upper_bound_ &&
      search_start_index >= *block_number_upper_bound_) {
    co_return nullptr;
  }

  auto it = blocks_.lower_bound(search_start_index);
  if (it != blocks_.end()) {
    co_return it->second.get();
  }

  // rcq_mtx_ is held by our caller across this suspension, so no other
  // coroutine observes the half-updated blocks_/upper-bound state below.
  auto db_res = co_await
      db_proxy.get_next_retrocausal_queue_block(block_id, addrkey,
                                                search_start_index);
  if (!db_res) {
    co_return db_res.error();
  }
  auto block = db_res.value();

  if (!block) {
    // cache that there isn't a block above search_start_index
    assert(!block_number_upper_bound_ ||
           search_start_index < *block_number_upper_bound_);
    block_number_upper_bound_ = search_start_index;
    co_return nullptr;
  }

  auto ptr = std::make_unique<RetrocausalQueueBlock>(*block);
  auto const *out = ptr.get();
  blocks_[block->block_number] = std::move(ptr);
  co_return out;
}

x23::Async<SystemStatus<const RetrocausalQueueBlock *>>
SnapshotStorageItem::get_next_rcq_block(uint64_t search_start_block) const
{
  // This method is only valid for RCQ items
  if (!std::holds_alternative<RetrocausalQueueEntry>(entry_)) {
    co_return nullptr;
  }
  auto const &rcq = std::get<RetrocausalQueueEntry>(entry_);
  x23::CoroGuard g;
  co_await g(rcq_mtx_);
  co_return co_await rcq.get_next_block(db_proxy_, block_id_, addrkey_,
                                        search_start_block);
}

x23::Async<SystemStatus<Nondeterministic<std::optional<QueueEntry>>>>
SnapshotStorageItem::get_concurrent_rcq_entry(uint32_t index) const
{
  {
    x23::CoroGuard g;
    co_await g(rcq_mtx_);
    if (index < concurrent_queue_entries_.size()) {
      co_return concurrent_queue_entries_[index];
    }
    if (index != concurrent_queue_entries_.size()) [[unlikely]] {
      error("unrecoverable: bad get_concurrent_rcq_entry access");
      co_return SystemError{SystemErrorCode::Unrecoverable};
    }
  }

  // Deliberately read without the lock held, then reconcile below: a racing
  // load of the same index issues a second RPC but must see the same answer,
  // which the mismatch check enforces.
  auto db_res = co_await
      db_proxy_.get_concurrent_retrocausal_queue_entry(block_id_, addrkey_,
                                                       index);
  if (!db_res) {
    co_return db_res.error();
  }
  auto entry = db_res.value();
  if (entry.has_value()) {
    x23::CoroGuard g;
    co_await g(rcq_mtx_);
    if (index == concurrent_queue_entries_.size()) {
      concurrent_queue_entries_.push_back(*entry);
    }
    else if (concurrent_queue_entries_[index] != *entry) [[unlikely]] {
      error("unrecoverable: mismatch in concurrent rcq return results");
      co_return SystemError{SystemErrorCode::Unrecoverable};
    }
  }
  co_return entry;
}

} // namespace hogd
