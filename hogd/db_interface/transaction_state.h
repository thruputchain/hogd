#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/container/synchronized_map.h"
#include "hogd/storage_item/storage_lock_util.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"

namespace hogd {

struct SubTransactionState {
  std::unordered_map<AddressAndKey, std::pair<StorageDelta, int128_t>> deltas_;
};

// Store data relevant to the 2PC process for one transaction.
class TransactionState {
public:
  TransactionState() = default;

  TransactionState(TransactionState const &) = delete;
  TransactionState &operator=(TransactionState const &) = delete;
  TransactionState(TransactionState &&) = default;
  TransactionState &operator=(TransactionState &&) = default;

  // Open a new subtransaction.  This is e.g. called when a transaction begins
  // a new try_invoke frame.
  // New add_delta operations will always be added to the most recent
  // subtransaction.
  void push_subtransaction();

  // Close the most recent `count` subtransactions.  This is e.g. called when
  // a call frame (that contained some number of try_invoke frames) reverts.
  void pop_subtransactions(uint32_t count);

  // Add a storage delta and associated lock for a given address+key pair.
  // The lock must have been acquired by the caller (via querying the
  // appropriate StorageItemDeltaManager). The delta will be added to the
  // current subtransaction. Any previous delta for this address+key in the
  // current subtransaction will be overwritten. The lock will be held until the
  // transaction completes. (TODO(geoff): impl early release, if necessary)
  void add_delta(AddressAndKey const &addrkey, StorageDelta &&delta,
                 MeteredStorageLock &&new_lock);

  std::optional<StorageDelta> get_delta(AddressAndKey const &addrkey) const;

  // Get the lock held by the current transaction for a given address+key pair.
  StorageLock get_lock(AddressAndKey const &addrkey);
  // Get the amount of storage cost expected as locked for this addrkey pair.
  int128_t get_cost(AddressAndKey const &addrkey) const;

  // Extract the results of the transaction, i.e. the set of all deltas and
  // associated locks that commit in the final view of the transaction. This
  // method must be called at most once per transaction.
  //
  // Each entry in the returned map is a pair of:
  // - the delta that was added for this address+key, or std::nullopt if the
  // delta was reverted.
  // - the lock that was added for this address+key (i.e. if the associated
  // delta is nullopt, we need to release the lock).
  std::unordered_map<AddressAndKey,
                     std::pair<std::optional<StorageDelta>, MeteredStorageLock>>
  extract_results();

private:
  // Stores the most recently applied delta for this transaction,
  // along with any held storage locks.
  // To revert a delta, overwrite the delta with a new delta (which would
  // be the previous delta, as calculated by the runtime.)
  //
  // TODO(geoff): this doesn't _release_ an acquired lock until the end of the
  // transaction, whether we commit or revert.  This may not be _optimal_, but
  // probably good enough. It's not an error to hold a lock for too
  // long.
  std::unordered_map<AddressAndKey, StorageLock> held_locks_;
  std::vector<SubTransactionState> subtransaction_stack_;
  bool finished_ = false;
};

// Manage the state of all transactions in a block.
// Lightweight wrapper around a synchronized map of TransactionState objects.
class TransactionStateManager {
public:
  // Get the state associated with one transaction.
  // Returns a pointer to the state and a unique lock on the state.
  // If the state does not exist, it is created.
  std::pair<TransactionState &, std::unique_lock<std::mutex>>
  get(TransactionID txid);

  // Returns the number of new transactions created.
  uint64_t new_transaction_count() const
  {
    return new_transaction_count_.load(std::memory_order_acquire);
  }
  uint64_t closed_transaction_count() const
  {
    return closed_transaction_count_.load(std::memory_order_acquire);
  }

  // Call when a transaction is committed or aborted.
  void increment_closed()
  {
    closed_transaction_count_.fetch_add(1, std::memory_order_release);
  }

  // Asserts that all opened transactions have been committed or aborted.
  void
  assert_all_closed() // NOLINT(readability-convert-member-functions-to-static)
      const
  {
    assert(new_transaction_count() == closed_transaction_count());
  }

private:
  using TransactionStateMap = SynchronizedMap<TransactionID, TransactionState>;
  TransactionStateMap tx_states_;
  std::atomic<uint64_t> new_transaction_count_{0};
  std::atomic<uint64_t> closed_transaction_count_{0};
};

} // namespace hogd
