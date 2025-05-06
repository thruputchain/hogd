#pragma once

#include <cassert>
#include <optional>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/db_interface/storage_view.h"
#include "hogd/db_interface/transaction_state.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

// Wrapper class to manage all of the state associated with one
// (pending or committed) block.
// Holds reference to a parent block as long as necessary;
// when the parent block is eventually committed to the database,
// the parent can be unlinked, at which point the reference is released.
//
// Note: the base database does not maintain versioning for blocks that do not
// commit and have no possibility of committing (i.e., some directly preceeding
// block had its sibling commit instead of it).
// Any pending modifications on a block whose sibling commits (from, e.g.,
// delayed RPCs or slow executors) may read invalid state (and give invalid
// try_reserve_storage_delta results) as a result.  However, these blocks will
// never commit anyways, so this does not affect correctness.
class BlockState {
public:
  BlockState(BlockState *parent)
    : storage_view_(parent->storage_view_.as_parent())
  {
    // if parent is committed to database, then we should
    // use the other constructor
    assert(!parent->is_committed_to_database());
  }

  BlockState(InMemoryDatabase *base_db) : storage_view_(base_db) {}

  BlockState(BlockState const &) = delete;
  BlockState &operator=(BlockState const &) = delete;
  BlockState(BlockState &&) = delete;
  BlockState &operator=(BlockState &&) = delete;

  // Writes this block's state to the base database.
  // After this call, any children can be unlinked.
  void commit_to_database();
  bool is_committed_to_database() const { return is_committed_to_database_; }

  // after this call, the object will read directly from the base database
  // instead of the parent.
  void unlink_parent(InMemoryDatabase *base_database);

  // It is an error to reserve a transaction delta for a txid and then
  // not call commit_transaction or abort_transaction before
  // ending the block.
  // Requirement: A transaction cannot try to reserve multiple deltas for the
  // same addrkey at the same time.  This is not enforced by the interface.
  std::optional<int128_t>
  try_reserve_storage_delta(TransactionID txid, AddressAndKey const &addrkey,
                            StorageDelta delta, StorageItemRPCFlagSet flags);
  void commit_transaction(TransactionID txid);
  void abort_transaction(TransactionID txid);

  void commit_contract_create(Contract const &contract);
  void commit_contract_deploy(ContractAddress const &addr,
                              SHA256Hash const &contract_hash);

  // parent block view is a read-only view of storage items.
  auto *get_snapshot_view() { return storage_view_.as_parent(); }

  // WARNING: nondeterministic
  Nondeterministic<std::optional<QueueEntry>>
  get_concurrent_retrocausal_queue_entry(AddressAndKey const &addrkey,
                                         uint32_t index);

  bool has_bad_item() const { return storage_view_.has_bad_item(); }

  void close_block() { storage_view_.close_block(); }
  bool is_closed() const { return storage_view_.is_closed(); }

  void push_subtransaction(TransactionID txid);
  void pop_subtransactions(TransactionID txid, uint32_t count);

private:
  StorageView storage_view_;
  TransactionStateManager tx_manager_;

  // nullptr if parent has been unlinked
  // Hold this shared_ptr to keep parent state alive.
  bool is_committed_to_database_ = false;
};

} // namespace hogd
