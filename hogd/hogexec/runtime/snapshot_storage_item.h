#pragma once

#include <cassert>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/status.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

// SnapshotStorageItem wraps a view of a database StorageItem,
// as it exists in the block's snapshot state,
// and provides lazy access to RetrocausalQueue components.
class SnapshotStorageItem {
public:
  // Construct from a StorageItem (by value)
  explicit SnapshotStorageItem(StorageItem item, AddressAndKey addrkey,
                               BlockID block_id, DatabaseProxy &db_proxy)
    : entry_([&item] -> std::variant<BytestringEntry, NNIntEntry,
                                     RetrocausalQueueEntry> {
        switch (item.type()) {
        case StorageItemType::kItemTypeBytestring:
          return BytestringEntry{std::move(item.bytestring())};
        case StorageItemType::kItemTypeNonnegativeInt:
          return NNIntEntry{from_xdr(item.nnint())};
        case StorageItemType::kItemTypeRetrocausalQueue:
          return RetrocausalQueueEntry();
        default:
          assert(false);
          throw std::runtime_error("should never get kDeleted from database");
        }
      }()),
      addrkey_(addrkey), block_id_(block_id), db_proxy_(db_proxy)
  {}

  // Delete copy and move
  SnapshotStorageItem(const SnapshotStorageItem &) = delete;
  SnapshotStorageItem &operator=(const SnapshotStorageItem &) = delete;
  SnapshotStorageItem(SnapshotStorageItem &&) = delete;
  SnapshotStorageItem &operator=(SnapshotStorageItem &&) = delete;

  SystemStatus<const RetrocausalQueueBlock *>
  get_next_rcq_block(uint64_t search_start_block);
  SystemStatus<Nondeterministic<std::optional<QueueEntry>>>
  get_concurrent_rcq_entry(uint32_t index);

  StorageItemType type() const
  {
    return std::visit(
        [](auto &&arg) -> StorageItemType { return arg.kItemType; }, entry_);
  }

  const Bytestring &bytestring() const
  {
    assert(type() == StorageItemType::kItemTypeBytestring);
    return std::get<BytestringEntry>(entry_).bytestring_;
  }

  int128_t nnint() const
  {
    assert(type() == StorageItemType::kItemTypeNonnegativeInt);
    return std::get<NNIntEntry>(entry_).base_value_;
  }

  auto const &addrkey() const { return addrkey_; }

private:
  struct BytestringEntry {
    Bytestring bytestring_;
    constexpr static StorageItemType kItemType =
        StorageItemType::kItemTypeBytestring;
  };

  struct NNIntEntry {
    const int128_t base_value_;
    constexpr static StorageItemType kItemType =
        StorageItemType::kItemTypeNonnegativeInt;
  };

  class RetrocausalQueueEntry {
  public:
    RetrocausalQueueEntry() = default;
    constexpr static StorageItemType kItemType =
        StorageItemType::kItemTypeRetrocausalQueue;

    // only returns from past blocks
    const RetrocausalQueueBlock *
    get_next_block(DatabaseProxy &db_proxy, BlockID block_id,
                   AddressAndKey const &addrkey,
                   uint64_t search_start_index) const;

  private:
    std::map<uint64_t, std::unique_ptr<RetrocausalQueueBlock>> blocks_;
    uint64_t max_observed_block_number_ = 0;
    // no need to repeatedly query for blocks above this number.
    std::optional<uint64_t> block_number_upper_bound_ = std::nullopt;
  };

  std::variant<BytestringEntry, NNIntEntry, RetrocausalQueueEntry> entry_;
  const AddressAndKey addrkey_;

  const BlockID block_id_;
  DatabaseProxy &db_proxy_;

  // Protects both the concurrent_queue_entries and the RetrocausalQueueEntry
  // contents.
  std::mutex rcq_mtx_;

  // Note: the elements of the concurrent read cache
  // can't be part of the variant, since the type of the storage item
  // might be different from the snapshot item.
  // (to be explicit: an address might have no element, but then
  // a queue might be read/consumed from that address).
  std::vector<QueueEntry> concurrent_queue_entries_;
};

} // namespace hogd
