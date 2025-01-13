#pragma once

#include <optional>
#include <variant>

#include "hogd/base/hashmap.h"
#include "hogd/hogdb/db/in_memory_database.h"
#include "hogd/hogdb/softstate/storage_item_frame.h"
#include "types.hh"

namespace hogd {

class StorageView {
public:
  bool try_reserve_storage_delta(AddressAndKey const &addrkey,
                                 StorageDelta const &delta);
  void revert_storage_delta(AddressAndKey const &addrkey,
                            StorageDelta const &delta);
  void commit_storage_delta(AddressAndKey const &addrkey,
                            StorageDelta const &delta);

  // the value of the item, before any modifications of the current block are
  // applied. Cannot be called after the block is closed.
  std::optional<StorageItem>
  get_snapshot_storage_item(AddressAndKey const &addrkey);
  // The value of the item, after all modifications of the current block are
  // applied. Cannot be called before the block is closed.
  std::optional<StorageItem>
  get_post_block_storage_item(AddressAndKey const &addrkey);

  // TODO(geoff): This isn't strictly necessary, but useful sanity check
  void close_block() { closed_ = true; }

private:
  static std::span<const std::byte> to_key(AddressAndKey const &addrkey);

  bool closed_ = false;
  HashMap<StorageItemFrame> modified_items_;
  std::variant<StorageView *, InMemoryDatabase *> parent_view_;
};

} // namespace hogd
