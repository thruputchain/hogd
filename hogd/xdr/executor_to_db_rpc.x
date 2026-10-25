// clang-format off

%#include "types.hh"
%#include "contract.hh"
%#include "transaction.hh"
%#include "storage_item.hh"
%#include "rpc_types.hh"
%#include "db_conflict.hh"

namespace hogd
{
// clang-format on

union ExpectedStorageItem switch (RPCError error)
{
case kRPCNone:
  WireStorageItem item;
default:
  void;
};

typedef FlaggedStorageDelta StorageDeltaBatch<>;

typedef Contract NewContractBatch<>;

/// Result of try_push_new_subtransaction. On kRPCStorageWriteFailed —
/// the nondeterministic transaction failure — carries the conflict
/// diagnostics at the level the database selected from the block's
/// execution mode. All other arms are void; kRPCNone costs one
/// discriminant word, same as the previous bare RPCError return.
union TryPushSubtransactionResult switch (RPCError error)
{
case kRPCStorageWriteFailed:
  DbConflictResult conflict;
default:
  void;
};

program DatabaseToExecutorInterface
{
  version DatabaseToExecutorInterfaceV1
  {
    // Subtransactions do not need their own unique IDs; every write from a
    // transaction must always be applied to the top-level subtransaction.
    //
    // Create a new subtransaction on top of the current (sub)transaction.
    // The root transaction, and any subtransactions, should finish
    // by calling this method with any deltas created during the (sub)
    // transaction.
    TryPushSubtransactionResult try_push_new_subtransaction(
      BlockID, TransactionID, StorageDeltaBatch) = 0;
    // We don't need a try_reserve_deltas method; use
    // try_push_new_subtransaction instead.
    RPCError commit_transaction(BlockID, TransactionID, NewContractBatch) = 1;
    RPCError abort_transaction(BlockID, TransactionID) = 2;

    // Revert one or more subtransactions.
    RPCError pop_subtransactions(BlockID, TransactionID, unsigned int) = 3;

    // Load the snapshot value of a storage item at the supplied address,
    // if it exists.
    ExpectedStorageItem get_snapshot_item(BlockID, AddressAndKey) = 100;

    // Returns the next block in the retrocausal queue at or after the
    // supplied block number. Returns kNoRetrocausalQueueBlock if there is no
    // element in the queue at or after the supplied block number (and before
    // the current block number).  Does not return data from the current
    // block, (the retrocausal part), which should be accessed via
    // get_concurrent_retrocausal_queue_entry().
    ExpectedRetrocausalQueueBlock get_next_retrocausal_queue_block(
      BlockID, AddressAndKey, unsigned hyper) = 101;

    // Returns the next unconsumed entry in the retrocausal queue for the
    // current block. The last argument is the index of the entry to return.
    // The queried order is nondeterministic.
    ExpectedQueueEntry get_concurrent_retrocausal_queue_entry(BlockID,
							      AddressAndKey,
							      unsigned int) =
    102;

    // TODO(geoff) consider whether we want to combine these or not.
    // Tradeoff is messages vs maybe sending redundant contracts.
    // Perhaps the sender could cache whether it expects the client to have
    // the contract, and optionally send accordingly.
    ExpectedContract get_contract(BlockID, SHA256Hash) = 202;
    ExpectedDeployment get_deployment(BlockID, ContractAddress) = 203;

    // Get the db tip the block executes on top of. The BlockHeight is the
    // height the caller opened the block with; the tip it builds on is the
    // one at that height minus one.
    //
    // The server answers off its committed tip alone -- no per-block
    // snapshot state -- so this cannot race the block's own creation:
    //   - tip height == height - 1: serve the tip;
    //   - tip height >= height: the chain has moved past the caller's
    //     block, so the block is dead: kRPCBadBlockID;
    //   - tip height < height - 1: only observable mid commit-publication;
    //     the server waits out the publication window and re-reads.
    //
    // The BlockID is the block being opened. Today's serving decision never
    // consults it -- there is exactly one committed tip, and the caller-side
    // proxy already refuses a served tip whose id equals the block it is
    // loading for -- but it stays on the wire so a pipelined server with
    // multiple tentative tips can select the parent chain the block was
    // created on.
    ExpectedDBTip get_db_tip(BlockID, BlockHeight) = 300;

    // Open a read-only viewing snapshot. Returns the new block ID and
    // the current committed tip. The caller must eventually call
    // close_viewing_snapshot to release the block tx.
    ExpectedViewingSnapshot open_viewing_snapshot(void) = 400;

    // Close a viewing snapshot opened by open_viewing_snapshot.
    RPCError close_viewing_snapshot(BlockID) = 401;
  }
  = 1;
}
= 44444444;

} // namespace hogd
