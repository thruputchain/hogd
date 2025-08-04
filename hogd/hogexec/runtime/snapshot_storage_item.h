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
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hogexec/runtime/item_view_type.h"
#include "hogd/hogexec/runtime/status.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

class DatabaseProxy;

// SnapshotStorageItem wraps a view of a database StorageItem,
// as it exists in the block's snapshot state,
// and provides lazy access to RetrocausalQueue components.
class SnapshotStorageItem {
public:
  // Construct from a StorageItem (by value)
  explicit SnapshotStorageItem(StorageItem item, AddressAndKey addrkey,
                               BlockID block_id, DatabaseProxy &db_proxy)
    : addrkey_(std::move(addrkey)),
      entry_(make_variant(std::move(item), addrkey_.key)), block_id_(block_id),
      db_proxy_(db_proxy)
  {
    assert(&db_proxy_);
  }

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

  std::optional<ItemViewType> type() const
  {
    return std::visit(
        [](auto &&arg) -> std::optional<ItemViewType> {
          if constexpr (requires(decltype(arg) item) { item.kItemType; }) {
            return arg.kItemType;
          }
          else {
            return std::nullopt;
          }
        },
        entry_);
  }

  const Bytestring &bytestring() const
  {
    assert(type() == ItemViewType::kBytestring);
    return std::get<BytestringEntry>(entry_).bytestring_;
  }

  int128_t nnint() const
  {
    assert(type() == ItemViewType::kNNInt);
    return std::get<NNIntEntry>(entry_).base_value_;
  }

  int128_t native_token_balance() const
  {
    assert(type() == ItemViewType::kNativeTokenBalance);
    return std::get<NativeTokenBalanceEntry>(entry_).base_value_;
  }

  auto const &replay_cache_metadata() const
  {
    assert(type() == ItemViewType::kReplayCache);
    return std::get<ReplayCacheEntry>(entry_).metadata_;
  }

  auto const &addrkey() const { return addrkey_; }

private:
  struct BytestringEntry {
    Bytestring bytestring_;
    constexpr static ItemViewType kItemType = ItemViewType::kBytestring;
  };

  struct NNIntEntry {
    const int128_t base_value_;
    constexpr static ItemViewType kItemType = ItemViewType::kNNInt;
  };

  // StorageSnapshotCache caches negative lookups as a DeletedEntry,
  // instead of a nullptr, because deleted entries might still be created
  // in this block and then we need to be able to have
  // the nondeterministic queue entry available for the address.
  struct DeletedEntry {};

  struct NativeTokenBalanceEntry {
    const int128_t base_value_;
    constexpr static ItemViewType kItemType = ItemViewType::kNativeTokenBalance;
  };

  class RetrocausalQueueEntry {
  public:
    RetrocausalQueueEntry() = default;

    RetrocausalQueueEntry(RetrocausalQueueEntry const &) = delete;
    RetrocausalQueueEntry &operator=(RetrocausalQueueEntry const &) = delete;
    RetrocausalQueueEntry(RetrocausalQueueEntry &&) = delete;
    RetrocausalQueueEntry &operator=(RetrocausalQueueEntry &&) = delete;

    constexpr static ItemViewType kItemType = ItemViewType::kRetrocausalQueue;

    // only returns from past blocks
    SystemStatus<const RetrocausalQueueBlock *>
    get_next_block(DatabaseProxy &db_proxy, BlockID block_id,
                   AddressAndKey const &addrkey, uint64_t search_start_index);

  private:
    std::map<uint64_t, std::unique_ptr<RetrocausalQueueBlock>> blocks_;
    // no need to repeatedly query for blocks above this number.
    std::optional<uint64_t> block_number_upper_bound_ = std::nullopt;
  };

  struct ReplayCacheEntry {
    ReplayCacheMetadata metadata_;
    constexpr static ItemViewType kItemType = ItemViewType::kReplayCache;
  };

  using variant_type =
      std::variant<BytestringEntry, NNIntEntry, RetrocausalQueueEntry,
                   DeletedEntry, NativeTokenBalanceEntry, ReplayCacheEntry>;
  static variant_type make_variant(StorageItem item, StorageKey const &db_key);
  static variant_type make_contract_key_variant(StorageItem item);

  const AddressAndKey addrkey_;
  variant_type entry_;

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
