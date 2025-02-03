// clang-format off

%#include "types.hh"
%#include "contract.hh"
%#include "transaction.hh"
%#include "storage_item.hh"
%#include "rpc_types.hh"

namespace hogd
{
  // clang-format on

  // The interface that the executor exposes to the coordinator.
  program ExectutorInterface
  {
    version ExecutorInterfaceV1
    {
      ExpectedProposalData propose_transaction(BlockID, SignedTransaction) = 0;
      RPCError validate_transaction(BlockID, ProposedTransaction) = 1;

      // Executor can be in one of three states:
      // None
      // Proposing
      // Validating
      // open_xyz_block() methods set the executor state, or return an error
      // if executor has already opened a block.
      //
      // finish_block() blocks until all running executor threads are finished,
      // and stops the executor from accepting new transactions.
      void open_proposal_block(BlockID) = 2;
      void open_validation_block(BlockID) = 3;
      void finish_block(BlockID) = 4;
    }
    = 1;
  }
  = 22222222;

  // The interface that the database exposes to the coordinator.
  program DatabaseToCoordInterface
  {
    version DatabaseToCoordInterfaceV1
    {
      // Called when creating a new block.
      // The first argument is the ID of the parent block.
      // The database will surface requests from the snapshot
      // that would be created if the parent block commits.
      // Parent block must be closed.
      RPCError open_database_snapshot(BlockID, BlockID) = 0;

      // Commits the blockID to disk.  Should only be called for e.g. finalized
      // blocks. At minimum block must be closed before being committed.
      // Any BlockID that is not a descendant of this block will return
      // an error if accessed. This BlockID must be the immediate descendant of
      // the most recently committed block.
      void commit_snapshot(BlockID) = 1;

      // Call to close a block. This indicates that no more transactions
      // will be added to this block. The block must have been previously opened
      // with open_database_snapshot().
      // A block cannot have a child opened before it is closed.
      // Error if the block has been pruned or contains an item in an invalid
      // state (due to e.g. a block validation failure).
      RPCError close_snapshot(BlockID) = 2;
    }
    = 1;
  }
  = 33333333;

  const kGenesisBlockID = 0;

  program DatabaseToExecutorInterface
  {
    version DatabaseToExecutorInterfaceV1
    {
      RPCError try_reserve_delta(BlockID, TransactionID, AddressAndKey,
                                 StorageDelta, RPCFlagSet) = 0;
      void commit_transaction(BlockID, TransactionID) = 1;
      void abort_transaction(BlockID, TransactionID) = 2;

      // Get items other than contents of the retrocausal queue.
      // If the element is actually a retrocausal queue, the item returned
      // has that type but does not give the contents.
      ExpectedStorageItem get_item(BlockID, AddressAndKey) = 3;

      // Returns the next block in the retrocausal queue at or after the
      // supplied block number. Returns kNoRetrocausalQueueBlock if there is no
      // element in the queue at or after the supplied block number (and before
      // the current block number).  Does not return data from the current
      // block, (the retrocausal part), which should be accessed via
      // get_concurrent_retrocausal_queue_entry().
      ExpectedRetrocausalQueueBlock get_next_retrocausal_queue_block(
          BlockID, AddressAndKey, unsigned hyper) = 4;

      // Returns the next unconsumed entry in the retrocausal queue for the
      // current block. The last argument is the index of the entry to return.
      // The queried order is nondeterministic.
      ExpectedQueueEntry get_concurrent_retrocausal_queue_entry(BlockID,
                                                                AddressAndKey,
                                                                unsigned int) =
          5;

      // Create a new contract.
      // Arguments:
      //   New Contract
      void commit_contract_create(BlockID, Contract) = 6;

      // Create a new deployment.
      // Arguments:
      //   ContractAddress
      //   SHA256Hash of contract to be deployed
      void commit_contract_deploy(BlockID, ContractAddress, SHA256Hash) = 7;

      // TODO(geoff) consider whether we want to combine these or not.
      // Tradeoff is messages vs maybe sending redundant contracts.
      // Perhaps the sender could cache whether it expects the client to have
      // the contract, and optionally send accordingly.
      ExpectedContract get_contract(BlockID, SHA256Hash) = 8;
      ExpectedDeployment get_deployment(BlockID, ContractAddress) = 9;
    }
    = 1;
  }
  = 44444444;

} // namespace hogd
