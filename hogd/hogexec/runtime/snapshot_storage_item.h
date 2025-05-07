#pragma once

#include <cassert>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    : entry_(make_variant(std::move(item))), addrkey_(addrkey),
      block_id_(block_id), db_proxy_(db_proxy)
  {}

  // Delete copy and move
  SnapshotStorageItem(const SnapshotStorageItem &) = delete;
  SnapshotStorageItem &operator=(const SnapshotStorageItem &) = delete;
  SnapshotStorageItem(SnapshotStorageItem &&) = delete;
  SnapshotStorageItem &operator=(SnapshotStorageItem &&) = delete;

  // We choose not to expose a queue() method,
  // instead wrapping calls to the queue in these methods.
  // This is because we might reasonably read from concurrent rcq entries,
  // even if the snapshot item is not a rcq item (it might be empty, or have
  // been deleted and then created as a queue this block).  As such, the
  // nondeterministic concurrent queue reads can't be stored in the variant, and
  // data structures that lazily load the queue have to be stored outside the
  // variant.  So it's easier (and slightly less memory) to wrap accesses in
  // these methods, rather than storing the same data access parameters in and
  // out of the variant.
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

  // StorageSnapshotCache caches negative lookups as a DeletedEntry,
  // instead of a nullptr, because deleted entries might still be created
  // in this block and then we need to be able to have
  // the nondeterministic queue entry available for the address.
  struct DeletedEntry {
    constexpr static StorageItemType kItemType =
        StorageItemType::kItemTypeDeleted;
  };

  class RetrocausalQueueEntry {
  public:
    RetrocausalQueueEntry() = default;

    RetrocausalQueueEntry(RetrocausalQueueEntry const &) = delete;
    RetrocausalQueueEntry &operator=(RetrocausalQueueEntry const &) = delete;
    RetrocausalQueueEntry(RetrocausalQueueEntry &&) = delete;
    RetrocausalQueueEntry &operator=(RetrocausalQueueEntry &&) = delete;

    constexpr static StorageItemType kItemType =
        StorageItemType::kItemTypeRetrocausalQueue;

    // only returns from past blocks
    SystemStatus<const RetrocausalQueueBlock *>
    get_next_block(DatabaseProxy &db_proxy, BlockID block_id,
                   AddressAndKey const &addrkey, uint64_t search_start_index);

  private:
    std::map<uint64_t, std::unique_ptr<RetrocausalQueueBlock>> blocks_;
    // no need to repeatedly query for blocks above this number.
    std::optional<uint64_t> block_number_upper_bound_ = std::nullopt;
  };

  static std::variant<BytestringEntry, NNIntEntry, RetrocausalQueueEntry,
                      DeletedEntry>
  make_variant(StorageItem item)
  {
    switch (item.type()) {
    case StorageItemType::kItemTypeBytestring:
      return std::variant<BytestringEntry, NNIntEntry, RetrocausalQueueEntry,
                          DeletedEntry>{std::in_place_type<BytestringEntry>,
                                        std::move(item.bytestring())};
    case StorageItemType::kItemTypeNonnegativeInt:
      return std::variant<BytestringEntry, NNIntEntry, RetrocausalQueueEntry,
                          DeletedEntry>{std::in_place_type<NNIntEntry>,
                                        from_xdr(item.nnint())};
    case StorageItemType::kItemTypeRetrocausalQueue:
      return std::variant<BytestringEntry, NNIntEntry, RetrocausalQueueEntry,
                          DeletedEntry>{
          std::in_place_type<RetrocausalQueueEntry>};
    case StorageItemType::kItemTypeDeleted:
      return std::variant<BytestringEntry, NNIntEntry, RetrocausalQueueEntry,
                          DeletedEntry>{std::in_place_type<DeletedEntry>};
    }
    assert(false);
    std::unreachable();
  }

  std::variant<BytestringEntry, NNIntEntry, RetrocausalQueueEntry, DeletedEntry>
      entry_;
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
