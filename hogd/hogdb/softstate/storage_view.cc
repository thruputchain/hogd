#include "hogd/hogdb/softstate/storage_view.h"

#include <cstddef>
#include <optional>
#include <span>

#include "hogd/crypto/crypto.h"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

std::span<const std::byte>
ParentBlockView::to_key(AddressAndKey const &addrkey)
{
  return std::as_bytes(std::span(reinterpret_cast<const uint8_t *>(&addrkey),
                                 sizeof(AddressAndKey)));
}

std::optional<StorageItem>
ParentBlockView::get_snapshot_storage_item(AddressAndKey const &addrkey)
{
  assert(!closed_);
  auto key = to_key(addrkey);

  auto local_res = modified_items_.modify_element(
      key, [](auto const &v) -> std::optional<StorageItem> {
        return v.get_snapshot_item();
      });

  if (local_res.has_value()) {
    return *local_res;
  }

  std::shared_lock lk(mtx_);
  return std::visit(
      [&addrkey](auto *parent) -> std::optional<StorageItem> {
        return parent->get_post_block_storage_item(addrkey);
      },
      parent_view_);
}

std::optional<StorageItem>
ParentBlockView::get_post_block_storage_item(AddressAndKey const &addrkey)
{
  assert(closed_);
  auto key = to_key(addrkey);

  auto local_res =
      modified_items_.modify_element(key,
                                     [](auto &v) -> std::optional<StorageItem> {
                                       return v.get_post_close_block_item();
                                     });

  if (local_res.has_value()) {
    return *local_res;
  }

  std::shared_lock lk(mtx_);
  return std::visit(
      [&addrkey](auto *parent) -> std::optional<StorageItem> {
        return parent->get_post_block_storage_item(addrkey);
      },
      parent_view_);
}

bool
StorageView::try_reserve_storage_delta(AddressAndKey const &addrkey,
                                       StorageDelta const &delta)
{
  assert(!closed_);
  auto key = to_key(addrkey);

  auto res = modified_items_.modify_element(key, [&delta](auto &v) -> bool {
    return v.try_reserve_delta(delta);
  });

  if (res.has_value()) {
    return *res;
  }

  std::optional<StorageItem> parent_value;
  {
    std::shared_lock lk(mtx_);
    // else, value was not present in cache.  Lookup from parent and emplace
    parent_value = std::visit(
        [&addrkey](auto *parent) -> std::optional<StorageItem> {
          return parent->get_post_block_storage_item(addrkey);
        },
        parent_view_);
  }
  modified_items_.emplace(key, addrkey, parent_value);

  res = modified_items_.modify_element(key, [&delta](auto &v) -> bool {
    return v.try_reserve_delta(delta);
  });

  return *res;
}

void
StorageView::revert_storage_delta(AddressAndKey const &addrkey,
                                  StorageDelta const &delta)
{
  assert(!closed_);
  auto key = to_key(addrkey);

  modified_items_.modify_element(key,
                                 [&delta](auto &v) { v.revert_delta(delta); });
}
void
StorageView::commit_storage_delta(AddressAndKey const &addrkey,
                                  StorageDelta const &delta)
{
  assert(!closed_);
  auto key = to_key(addrkey);

  modified_items_.modify_element(key,
                                 [&delta](auto &v) { v.commit_delta(delta); });
}

void
StorageView::unlink_parent(InMemoryDatabase *new_parent)
{
  assert(closed_);
  std::unique_lock lk(mtx_);
  parent_view_.emplace<InMemoryDatabase *>(new_parent);
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

} // namespace hogd
