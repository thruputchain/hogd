#include "hogd/hogexec/runtime/storage_snapshot_cache.h"

#include <cassert>
#include <memory>
#include <optional>

#include "contract.hh"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

StorageSnapshotCacheEntry::StorageSnapshotCacheEntry(
    const StorageItem &wire_item, DatabaseProxy &db_proxy, BlockID block_id)
{
  switch (wire_item.type()) {
  case StorageItemType::kBytestring:
    entry_.emplace<BytestringEntry>(wire_item.bytestring());
    break;
  case StorageItemType::kNonnegativeInt:
    entry_.emplace<NNIntEntry>(from_xdr(wire_item.nnint()));
    break;
  case StorageItemType::kRetrocausalQueue:
    entry_.emplace<RetrocausalQueueEntry>(db_proxy, block_id);
    break;
  case StorageItemType::kDeleted:
    assert(false);
    exit(EXIT_FAILURE);
  }
}

StorageSnapshotCacheEntry::RetrocausalQueueEntry::RetrocausalQueueEntry(
    DatabaseProxy &db_proxy, BlockID block_id)
  : db_proxy_(db_proxy), block_id_(block_id)
{}

const RetrocausalQueueBlock *
StorageSnapshotCacheEntry::RetrocausalQueueEntry::get_next_block(
    AddressAndKey const &addrkey, uint64_t search_start_index)
{
  auto it = blocks_.upper_bound(search_start_index);
  if (it != blocks_.end()) {
    return it->second.get();
  }

  auto db_res = db_proxy_.get_retrocausal_queue_block(block_id_, addrkey,
                                                      search_start_index);

  if (db_res) {
    auto block = std::make_unique<RetrocausalQueueBlock>(std::move(*db_res));
    auto *out = block.get();
    blocks_.emplace(search_start_index, std::move(block));
    return out;
  }
  return nullptr;
}

std::optional<QueueEntry>
StorageSnapshotCacheEntry::RetrocausalQueueEntry::get_concurrent_entry(
    AddressAndKey const &addrkey, uint64_t index)
{
  if (index < concurrent_items_.size()) {
    return concurrent_items_[index];
  }

  auto next_item =
      db_proxy_.get_concurrent_retrocausal_queue_entry(block_id_, addrkey,
                                                       index);
  if (next_item) {
    // don't cache a negative result, as a concurrent transaction might insert
    // something before block close.
    concurrent_items_.push_back(*next_item);
  }
  return next_item;
}

StorageItemType
StorageSnapshotCacheEntry::type() const
{
  return std::visit([](auto const &entry) { return entry.kItemType; }, entry_);
}

Bytestring const &
StorageSnapshotCacheEntry::bytestring() const
{
  assert(std::holds_alternative<BytestringEntry>(entry_));
  return std::get<BytestringEntry>(entry_).bytestring_;
}

int128_t
StorageSnapshotCacheEntry::nnint() const
{
  assert(std::holds_alternative<NNIntEntry>(entry_));
  return std::get<NNIntEntry>(entry_).base_value_;
}

std::optional<SHA256Hash>
StorageSnapshotCache::try_get_deployed_contract_hash(
    ContractAddress const &addr)
{
  auto key = DeploymentMapEntry::get_key(addr);
  if (auto res = deployment_cache_.modify_element(key, [](auto const &v) {
        return v.hash_;
      })) {
    return *res;
  }

  // cache positive and negative lookups
  auto db_res = db_proxy_.get_deployment(block_id_, addr);
  deployment_cache_.emplace(key, addr, db_res);
  return db_res;
}

std::shared_ptr<const Contract>
StorageSnapshotCache::try_get_contract_by_hash(SHA256Hash const &hash)
{

  auto key = ContractMapEntry::get_key(hash);
  if (auto res = contract_cache_.modify_element(key, [](auto const &v) {
        return v.contract_;
      })) {
    return *res;
  }

  // cache positive and negative lookups
  auto db_res = db_proxy_.get_contract(block_id_, hash);
  contract_cache_.emplace(key, hash, db_res);
  return db_res;
}

const StorageSnapshotCacheEntry *
StorageSnapshotCache::try_get_snapshot_item(AddressAndKey const &addrkey)
{
  auto key = StorageItemMapEntry::get_key(addrkey);
  if (auto res = snapshot_item_cache_.modify_element(key, [](auto const &v) {
        return v.entry_.get();
      })) {
    return *res;
  }

  // cache positive and negative lookups
  auto db_res = db_proxy_.get_item(block_id_, addrkey);
  std::unique_ptr<StorageSnapshotCacheEntry> entry =
      db_res ? std::make_unique<StorageSnapshotCacheEntry>(*db_res, db_proxy_,
                                                           block_id_)
             : nullptr;
  auto const *out = entry.get();

  snapshot_item_cache_.emplace(key, addrkey, std::move(entry));
  return out;
}
} // namespace hogd
