#include "hogd/hogdb/softstate/storage_view.h"

namespace hogd {

std::span<const std::byte>
StorageView::to_key(AddressAndKey const &addrkey)
{
  return std::as_bytes(std::span(reinterpret_cast<const uint8_t *>(&addrkey),
                                 sizeof(AddressAndKey)));
}

std::optional<StorageItem>
StorageView::get_snapshot_storage_item(AddressAndKey const &addrkey)
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

  std::shared_lock lk(mtx);
  return std::visit(
      [&addrkey](auto *parent) -> std::optional<StorageItem> {
        return parent->get_post_block_storage_item(addrkey);
      },
      parent_view_);
}

std::optional<StorageItem>
StorageView::get_post_block_storage_item(AddressAndKey const &addrkey)
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

  std::shared_lock lk(mtx);
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
    std::shared_lock lk(mtx);
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
  std::unique_lock lk(mtx);
  parent_view_.emplace<InMemoryDatabase *>(new_parent);
}

} // namespace hogd
