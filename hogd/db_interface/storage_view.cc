#include "hogd/db_interface/storage_view.h"

#include <cassert>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>
#include <variant>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mockdb/in_memory_database.h"
#include "hogd/storage_item/storage_item_delta_manager.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "hogd/storage_item/storage_lock_util.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"

namespace hogd {

// NOLINTBEGIN(misc-no-recursion)

const StorageItemWrapper *
SnapshotStorageView::get_snapshot_storage_item(AddressAndKey const &addrkey)
{
  {
    // Check cache of modified items first
    auto [item, ul] = modified_items_.find(addrkey);

    if (item)
      return item->get_snapshot_item().get();
  }

  // If not found in modified items, check parent
  auto res = get_parent_storage_item(addrkey);

  // negative and positive lookups are cached
  modified_items_.insert(addrkey,
                         StorageItemDeltaManager(res, &invalid_state_flag_,
                                                 mode_));
  return res.get();
}

std::shared_ptr<const StorageItemWrapper>
SnapshotStorageView::get_parent_storage_item(AddressAndKey const &addrkey)
{
  std::shared_lock lk(mtx_);
  return std::visit(
      [&addrkey](auto *parent) {
        return parent->get_post_block_storage_item(addrkey);
      },
      parent_view_);
}

std::shared_ptr<const StorageItemWrapper>
ParentBlockView::get_post_block_storage_item(AddressAndKey const &addrkey)
{
  assert(closed_);
  {
    // Check modified items first
    auto [item, ul] = modified_items_.find(addrkey);

    if (item)
      return item->get_post_close_block_item();
  }

  // If not found in modified items, check parent
  // This is after the block is closed, so we don't need to cache results
  // since no new deltas will be added.
  return get_parent_storage_item(addrkey);
}
std::optional<MeteredStorageLock>
StorageView::try_reserve_storage_delta(AddressAndKey const &addrkey,
                                       StorageDelta const &delta,
                                       StorageLock const &lock,
                                       StorageItemRPCFlagSet flags)
{
  assert(!closed_);

  auto mod_lambda = [&delta, &lock,
                     &flags](auto &v) -> std::optional<MeteredStorageLock> {
    if (flags &
        std::to_underlying(StorageItemRPCFlags::kStorageExclusiveLock)) {
      if (auto r = v->try_acquire_exclusive_lock(
              delta, lock.lock_type(),
              StorageLockType::kPermanentExclusiveLock)) {
        return r;
      }
      return std::nullopt;
    }
    // otherwise regular work path
    return v->try_reserve(delta, lock);
  };

  {
    auto [item, ul] = modified_items_.find(addrkey);
    if (item) {
      return mod_lambda(item);
    }
  }

  auto parent_item = get_parent_storage_item(addrkey);
  auto [item, ul] = modified_items_.insert(
      addrkey,
      StorageItemDeltaManager(parent_item, &invalid_state_flag_, mode_));
  return mod_lambda(item);
}

void
StorageView::revert_storage_lock(AddressAndKey const &addrkey,
                                 StorageLock const &lock)
{
  assert(!closed_);
  auto [item, ul] = modified_items_.find(addrkey);
  assert(item);
  item->revert(lock);
}

int128_t
StorageView::commit_storage_delta(AddressAndKey const &addrkey,
                                  StorageDelta const &delta,
                                  MeteredStorageLock const &lock)
{
  assert(!closed_);
  auto [item, ul] = modified_items_.find(addrkey);
  assert(item);
  return item->commit(delta, lock);
}

void
StorageView::unlink_parent(InMemoryDatabase *new_parent)
{
  std::unique_lock lk(mtx_);
  parent_view_.emplace<InMemoryDatabase *>(new_parent);
}

// After this function, this object could be dropped.
void
StorageView::commit_to_database()
{
  assert(closed_);
  assert(std::holds_alternative<InMemoryDatabase *>(parent_view_));
  // parent_view is an InMemoryDatabase, so no need to acquire the lock.
  // Once parent_view is an InMemoryDatabase, it will never be changed again.
  auto *base_database = std::get<InMemoryDatabase *>(parent_view_);

  modified_items_.for_each_parallel([base_database](auto &v) {
    base_database->write_item(v.first, v.second.get_post_close_block_item());
  });

  auto contract_creates = contract_creates_.take();
  for (auto &[hash, contract] : *contract_creates) {
    base_database->write_contract(hash, contract);
  }
  auto contract_deployments = contract_deployments_.take();
  for (auto &[addr, hash] : *contract_deployments) {
    base_database->write_deployment(addr, hash);
  }
}

std::optional<SHA256Hash>
SnapshotStorageView::get_snapshot_deployment(ContractAddress const &addr)
{
  std::shared_lock lock(mtx_);
  return std::visit(
      [&addr](auto *parent) { return parent->get_post_block_deployment(addr); },
      parent_view_);
}

std::shared_ptr<const Contract>
SnapshotStorageView::get_snapshot_contract(SHA256Hash const &hash)
{
  std::shared_lock lock(mtx_);
  return std::visit(
      [&hash](auto *parent) { return parent->get_post_block_contract(hash); },
      parent_view_);
}

// Read from post block snapshot data, after all operations in the block
// are applied.
// Cannot be called before the block is closed.
std::optional<SHA256Hash>
ParentBlockView::get_post_block_deployment(ContractAddress const &addr)
{
  assert(closed_);
  {
    auto m = contract_deployments_.take();
    if (auto it = m->find(addr); it != m->end()) {
      return it->second;
    }
  }

  return get_snapshot_deployment(addr);
}

std::shared_ptr<const Contract>
ParentBlockView::get_post_block_contract(SHA256Hash const &hash)
{
  assert(closed_);
  {
    auto m = contract_creates_.take();
    if (auto it = m->find(hash); it != m->end()) {
      return it->second;
    }
  }

  return get_snapshot_contract(hash);
}

void
StorageView::commit_contract_create(Contract const &contract)
{
  assert(!closed_);
  auto hash = sha256(contract);
  contract_creates_->emplace(hash, std::make_shared<const Contract>(contract));
}

void
StorageView::commit_contract_deploy(ContractAddress const &addr,
                                    SHA256Hash const &contract_hash)
{
  assert(!closed_);
  contract_deployments_->emplace(addr, contract_hash);
}

Nondeterministic<std::optional<QueueEntry>>
StorageView::get_concurrent_retrocausal_queue_entry(
    AddressAndKey const &addrkey, uint32_t index)
{
  assert(!closed_);

  auto [item, ul] = modified_items_.find(addrkey);
  if (item) {
    return item->get_concurrent_retrocausal_queue_entry(index);
  }

  return std::nullopt;
}

// NOLINTEND(misc-no-recursion)

} // namespace hogd
