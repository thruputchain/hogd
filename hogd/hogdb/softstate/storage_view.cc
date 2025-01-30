#include "hogd/hogdb/softstate/storage_view.h"

#include <cstddef>
#include <optional>
#include <span>

#include "hogd/crypto/crypto.h"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"

namespace hogd {

std::span<const std::byte>
ParentBlockView::to_key(AddressAndKey const &addrkey)
{
  return std::as_bytes(std::span(reinterpret_cast<const uint8_t *>(&addrkey),
                                 sizeof(AddressAndKey)));
}

std::optional<StorageItemWrapper>
ParentBlockView::get_snapshot_storage_item(AddressAndKey const &addrkey)
{
  assert(!closed_);
  auto key = to_key(addrkey);

  auto local_res = modified_items_.modify_element(
      key, [](auto const &v) -> std::optional<StorageItemWrapper> {
        return v.get_snapshot_item();
      });

  if (local_res.has_value()) {
    return *local_res;
  }

  std::shared_lock lk(mtx_);
  return std::visit(
      [&addrkey](auto *parent) -> std::optional<StorageItemWrapper> {
        return parent->get_post_block_storage_item(addrkey);
      },
      parent_view_);
}

std::optional<StorageItemWrapper>
ParentBlockView::get_post_block_storage_item(AddressAndKey const &addrkey)
{
  assert(closed_);
  auto key = to_key(addrkey);

  auto local_res = modified_items_.modify_element(key, [](auto &v) {
    return v.get_post_close_block_item();
  });

  if (local_res.has_value()) {
    return *local_res;
  }

  std::shared_lock lk(mtx_);
  return std::visit(
      [&addrkey](auto *parent) {
        return parent->get_post_block_storage_item(addrkey);
      },
      parent_view_);
}

std::optional<StorageLock>
StorageView::try_reserve_storage_delta(AddressAndKey const &addrkey,
                                       StorageDelta const &delta,
                                       StorageLock const &lock,
                                       RPCFlagSet flags)
{
  assert(!closed_);
  auto key = to_key(addrkey);

  auto mod_lambda = [&delta, &lock,
                     &flags](auto &v) -> std::optional<StorageLock> {
    if (flags & std::to_underlying(RPCFlags::kExclusiveLock)) {
      if (v.try_acquire_exclusive_lock(lock.lock_type())) {
        return StorageLock(StorageLockType::kPermanentExclusiveLock);
      }
      return std::nullopt;
    }
    // otherwise regular work path
    return v.try_reserve(delta, lock);
  };

  auto res = modified_items_.modify_element(key, mod_lambda);
  if (res.has_value()) {
    // modified_items_ found the key.
    // *res is std::optional<StorageLock>: has_value() is true if the operation
    // succeeds, otherwise it is nullopt
    return *res;
  }

  std::optional<StorageItemWrapper> parent_value;
  {
    std::shared_lock lk(mtx_);
    // else, value was not present in cache.  Lookup from parent and emplace
    parent_value = std::visit(
        [&addrkey](auto *parent) {
          return parent->get_post_block_storage_item(addrkey);
        },
        parent_view_);
  }
  modified_items_.emplace(key, addrkey, parent_value);

  res = modified_items_.modify_element(key, mod_lambda);
  // lookup must succeed, we just added it
  assert(res.has_value());
  return *res;
}

void
StorageView::revert_storage_lock(AddressAndKey const &addrkey,
                                 StorageLock const &lock)
{
  assert(!closed_);
  auto key = to_key(addrkey);

  modified_items_.modify_element(key, [&lock](auto &v) { v.revert(lock); });
}
void
StorageView::commit_storage_delta(AddressAndKey const &addrkey,
                                  StorageDelta const &delta,
                                  StorageLock const &lock)
{
  assert(!closed_);
  auto key = to_key(addrkey);

  [[maybe_unused]]
  auto res = modified_items_.modify_element(key, [&delta, &lock](auto &v) {
    v.commit(delta, lock);
  });
  assert(res);
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

  modified_items_.map_apply([base_database](auto &v) {
    base_database->write_item(v.get_addr_key(), v.get_post_close_block_item());
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
ParentBlockView::get_snapshot_deployment(ContractAddress const &addr)
{
  std::shared_lock lock(mtx_);
  return std::visit(
      [&addr](auto *parent) { return parent->get_post_block_deployment(addr); },
      parent_view_);
}

std::shared_ptr<const Contract>
ParentBlockView::get_snapshot_contract(SHA256Hash const &hash)
{
  std::printf("ParentBlockView::get_snapshot_contract %s\n",
              x23::show(hash).c_str());
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
  std::printf("ParentBlockView::get_post_block_contract %s\n",
              x23::show(hash).c_str());
  assert(closed_);
  {
    auto m = contract_creates_.take();
    if (auto it = m->find(hash); it != m->end()) {
      std::printf(
          "ParentBlockView::get_post_block_contract found in cache %s\n",
          x23::show(hash).c_str());
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
  std::printf("StorageView::commit_contract_create %p %s\n", this,
              x23::show(hash).c_str());
  contract_creates_->emplace(hash, std::make_shared<const Contract>(contract));
}
void
StorageView::commit_contract_deploy(ContractAddress const &addr,
                                    SHA256Hash const &contract_hash)
{
  assert(!closed_);
  contract_deployments_->emplace(addr, contract_hash);
}

} // namespace hogd
