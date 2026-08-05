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

#include "address_auth.hh"
#include "hogd/base/log.h"
#include "hogd/base/thread_checked_mutex.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/item/item_view_type.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class DatabaseProxy;

// SnapshotStorageItem wraps a view of a database StorageItem,
// as it exists in the block's snapshot state,
// and provides lazy access to RetrocausalQueue components.
class SnapshotStorageItem {
public:
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
  get_next_rcq_block(uint64_t search_start_block) const;
  SystemStatus<Nondeterministic<std::optional<QueueEntry>>>
  get_concurrent_rcq_entry(uint32_t index) const;

  RetrocausalQueueMetadata const &retrocausal_queue_metadata() const
  {
    CHECK(type() == ItemViewType::kRetrocausalQueue);
    return std::get<RetrocausalQueueEntry>(entry_).metadata();
  }

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

  const AddressAuthData &auth_data() const
  {
    assert(type() == ItemViewType::kAddressAuthData);
    return std::get<AddressAuthDataEntry>(entry_).address_auth_data_;
  }

  auto const &addrkey() const { return addrkey_; }

  // We must have already loaded up to count retrocausally consumed entries from
  // the db.
  x23::Vec<QueueEntry> get_retrocausally_consumed_entries(uint32_t count) const
  {
    std::scoped_lock ul(rcq_mtx_);
    CHECK_LE(count, concurrent_queue_entries_.size());
    return x23::Vec<QueueEntry>(concurrent_queue_entries_.begin(),
                                concurrent_queue_entries_.begin() + count);
  }

private:
#ifdef NDEBUG
  using mutex_type = std::mutex;
#else
  using mutex_type = ThreadCheckedMutex<std::mutex>;
#endif

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

  struct AddressAuthDataEntry {
    AddressAuthData address_auth_data_;
    constexpr static ItemViewType kItemType = ItemViewType::kAddressAuthData;
  };

  class RetrocausalQueueEntry {
  public:
    RetrocausalQueueEntry(RetrocausalQueueMetadata metadata)
      : metadata_(std::move(metadata))
    {}
    RetrocausalQueueEntry() = default;

    // Non-copyable (owns unique_ptr blocks), but movable: make_variant builds
    // the entry and moves it into the (immovable) SnapshotStorageItem's entry_
    // during construction, before it is cached or accessed under rcq_mtx_.
    RetrocausalQueueEntry(RetrocausalQueueEntry const &) = delete;
    RetrocausalQueueEntry &operator=(RetrocausalQueueEntry const &) = delete;
    RetrocausalQueueEntry(RetrocausalQueueEntry &&) noexcept = default;
    RetrocausalQueueEntry &
    operator=(RetrocausalQueueEntry &&) noexcept = default;

    constexpr static ItemViewType kItemType = ItemViewType::kRetrocausalQueue;

    RetrocausalQueueMetadata const &metadata() const { return metadata_; }

    // only returns from past blocks
    SystemStatus<const RetrocausalQueueBlock *>
    get_next_block(DatabaseProxy &db_proxy, BlockID block_id,
                   AddressAndKey const &addrkey, uint64_t search_start_index,
                   std::unique_lock<mutex_type> &ul) const;

  private:
    // INVARIANT: These should only be modified while holding the rcq_mtx_ lock.
    // These are only lazily loaded from the db; as such, concurrent loads might
    // be interleaved in arbitrary ways, but will still return the same results.
    mutable std::map<uint64_t, std::unique_ptr<RetrocausalQueueBlock>> blocks_;
    // no need to repeatedly query for blocks above this number.
    mutable std::optional<uint64_t> block_number_upper_bound_ = std::nullopt;
    RetrocausalQueueMetadata metadata_;
  };

  struct ReplayCacheEntry {
    constexpr static ItemViewType kItemType = ItemViewType::kReplayCache;
  };

  using variant_type =
      std::variant<BytestringEntry, NNIntEntry, RetrocausalQueueEntry,
                   DeletedEntry, NativeTokenBalanceEntry, ReplayCacheEntry,
                   AddressAuthDataEntry>;

public:
  // Map a wire StorageItem under a given key to its entry variant. Returns
  // Unrecoverable for an unsupported/unexpected (key_type, item_type)
  // combination. Single source of truth for the mapping; the cache calls it
  // and surfaces the status before constructing the (immovable) item.
  static SystemStatus<variant_type> make_variant(WireStorageItem item,
                                                 StorageKey const &db_key);

  // Construct from an already-mapped entry (see make_variant).
  SnapshotStorageItem(variant_type entry, AddressAndKey addrkey,
                      BlockID block_id, DatabaseProxy &db_proxy);

private:
  static SystemStatus<variant_type>
  make_contract_key_variant(WireStorageItem item);

  const AddressAndKey addrkey_;
  variant_type entry_;

  const BlockID block_id_;
  DatabaseProxy &db_proxy_;

  // Protects both the concurrent_queue_entries and the RetrocausalQueueEntry
  // contents.
  //
  // This mutex is checked (currently only in debug builds) for migration across
  // threads because it is held accesses to hog_db via the DatabaseProxy, which
  // currently uses an SRpcClient. An SRpcClient is safe inside DatabaseProxy,
  // as calls suspend instead of migrating across threads. If we ever change the
  // internals of DatabaseProxy, this mutex would cause
  mutable mutex_type rcq_mtx_;

  // Note: the elements of the concurrent read cache
  // can't be part of the variant, since the type of the storage item
  // might be different from the snapshot item.
  // (to be explicit: an address might have no element, but then
  // a queue might be read/consumed from that address).
  mutable std::vector<QueueEntry> concurrent_queue_entries_;
};

} // namespace hogd
