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
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/item/item_view_type.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
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
  // Coroutines: both lazily load from hog_db under rcq_mtx_, which is a
  // CoroLock, so acquiring it suspends the caller instead of parking the
  // thread.
  x23::Async<SystemStatus<const RetrocausalQueueBlock *>>
  get_next_rcq_block(uint64_t search_start_block) const;
  x23::Async<SystemStatus<Nondeterministic<std::optional<QueueEntry>>>>
  get_concurrent_rcq_entry(uint32_t index) const;

private:
  // The two locked halves of get_concurrent_rcq_entry, split so that each one
  // holds rcq_mtx_ for a single straight-line region it can leave through
  // reset_and_return (see the note on rcq_mtx_).  The database read runs
  // between them with the lock released.
  //
  // Returns the entry already recorded at [index], nullopt if [index] is the
  // next one to load, or Unrecoverable for an out-of-order access.
  x23::Async<SystemStatus<std::optional<QueueEntry>>>
  peek_concurrent_entry(uint32_t index) const;
  // Records [entry] at [index], or checks it against what a racing load
  // already recorded there.
  x23::Async<SystemStatus<void>>
  record_concurrent_entry(uint32_t index, QueueEntry entry) const;

public:
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
  // the db.  A coroutine only because rcq_mtx_ is a CoroLock; it touches no
  // database and never suspends once it holds the lock.
  //
  // Releases the lock through reset_and_return rather than letting ~CoroGuard
  // do it -- see the note on rcq_mtx_.
  x23::Async<x23::Vec<QueueEntry>>
  get_retrocausally_consumed_entries(uint32_t count) const
  {
    x23::CoroGuard g;
    co_await g(rcq_mtx_);
    CHECK_LE(count, concurrent_queue_entries_.size());
    co_await g.reset_and_return(
        x23::Vec<QueueEntry>(concurrent_queue_entries_.begin(),
                             concurrent_queue_entries_.begin() + count));
    panic("unreachable");
  }

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

    // only returns from past blocks.  The caller must hold rcq_mtx_ for the
    // whole call: this suspends on the database read with the lock held, which
    // a CoroLock (unlike a std::mutex) supports.
    x23::Async<SystemStatus<const RetrocausalQueueBlock *>>
    get_next_block(DatabaseProxy &db_proxy, BlockID block_id,
                   AddressAndKey const &addrkey,
                   uint64_t search_start_index) const;

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
  // A CoroLock rather than a std::mutex because it is held across the
  // DatabaseProxy reads below, which now suspend.  A std::mutex cannot survive
  // that on either count: the coroutine may resume on a different thread than
  // the one that locked it (unlocking a std::mutex from another thread is UB),
  // and because this item is shared between the concurrent transactions of a
  // block, a second transaction resuming on this thread would block the whole
  // worker waiting for a lock whose holder is parked on an RPC.  Acquiring a
  // CoroLock suspends the waiter instead, so neither happens.
  //
  // Every holder below releases it with `co_await g.reset_and_return(...)` and
  // never by letting ~CoroGuard run.  The destructor path calls CoroLock's
  // unlock(), which resumes the next waiter as a plain nested call on the
  // current stack; under contention that builds an unbounded
  // unlock->resume->unlock chain and overruns the thread stack.
  // reset_and_return hands the next waiter back from await_suspend instead, so
  // the resume is a symmetric transfer that reuses this frame rather than
  // stacking on it (cf. EventRPCContext::get_context_slow).
  mutable x23::CoroLock rcq_mtx_;

  // Note: the elements of the concurrent read cache
  // can't be part of the variant, since the type of the storage item
  // might be different from the snapshot item.
  // (to be explicit: an address might have no element, but then
  // a queue might be read/consumed from that address).
  mutable std::vector<QueueEntry> concurrent_queue_entries_;
};

} // namespace hogd
