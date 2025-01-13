#pragma once

#include <cassert>
#include <optional>
#include <shared_mutex>
#include <variant>

#include "hogd/base/hashmap.h"
#include "hogd/hogdb/db/in_memory_database.h"
#include "hogd/hogdb/softstate/storage_item_frame.h"
#include "types.hh"

namespace hogd {

// On threadsafety:
// The methods that change/add storage deltas cannot be called after
// close_block, and get_post_block_storage_item cannot be called
// before close_block.
//
// The view cannot have a child view opened until close_block is called.
class StorageView {
public:
  StorageView(InMemoryDatabase *root_db)
    : modified_items_(8, 8), parent_view_(root_db)
  {}
  StorageView(StorageView *parent_view)
    : modified_items_(8, 8), parent_view_(parent_view)
  {}

  // Disable move and copy
  StorageView(StorageView const &) = delete;
  StorageView &operator=(StorageView const &) = delete;
  StorageView(StorageView &&) = delete;
  StorageView &operator=(StorageView &&) = delete;

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
  void assert_block_closed() { assert(closed_); }

  // Parent view has been committed to the persistent database.
  // Rather than jump through the parent StorageView,
  // at that point, we can just directly access the persistent database.
  void unlink_parent(InMemoryDatabase *new_parent);

private:
  static std::span<const std::byte> to_key(AddressAndKey const &addrkey);

  bool closed_ = false;
  HashMap<StorageItemFrame> modified_items_;

  std::shared_mutex mtx;
  std::variant<StorageView *, InMemoryDatabase *> parent_view_;
};

} // namespace hogd
