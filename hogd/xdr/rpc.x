// clang-format off

%#include "types.hh"
%#include "contract.hh"
%#include "transaction.hh"
%#include "storage_item.hh"
%#include "rpc_types.hh"

namespace hogd
{
  // clang-format on

  // Contract deploy and create are methods that cannot have conflicts,
  // and can only fail if there are catastrophic errors (like OOM) that would
  // cause us to abort the whole block.  As such, _at the moment_, they do not
  // have a 2PC interface, nor a TXID, although we could add those if we find a
  // reason to do so.
  program ContractStorageInterface
  {
    version ContractStorageInterfaceV1
    {
      // Create a new contract.
      // Arguments:
      //   New Contract
      // No-op whenever error is not kNone
      RPCError commit_contract_create(Contract) = 1;

      // Create a new deployment.
      // Arguments:
      //   ContractAddress
      //   SHA256Hash of contract to be deployed
      // No-op whenever error is not kNone
      RPCError commit_contract_deploy(ContractAddress, SHA256Hash) = 2;

      // TODO(geoff) consider whether we want to combine these or not.
      // Tradeoff is messages vs maybe sending redundant contracts.
      // Perhaps the sender could cache whether it expects the client to have
      // the contract, and optionally send accordingly.
      ExpectedContract get_contract(SHA256Hash) = 3;
      ExpectedDeployment get_deployment(ContractAddress) = 4;
    }
    = 1;
  }
  = 11111111;

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
      void open_database_snapshot(BlockID, BlockID) = 0;

      void close_snapshot(BlockID) = 1;

      // Commits the blockID to disk.  Should only be called for e.g. finalized
      // blocks. Any BlockID that is not a descendant of this block will return
      // an error if accessed. This BlockID must be the immediate descendant of
      // the most recently committed block.
      void commit_snapshot(BlockID) = 2;
    }
    = 1;
  }
  = 33333333;

  program DatabaseToExecutorInterface
  {
    version DatabaseToExecutorInterfaceV1
    {
      RPCError try_reserve_delta(BlockID, TransactionID, AddressAndKey,
                                 StorageDelta) = 0;
      void commit_transaction(BlockID, TransactionID) = 1;
      void abort_transaction(BlockID, TransactionID) = 2;

      ExpectedStorageItem get_item(BlockID, AddressAndKey) = 3;
    }
    = 1;
  }
  = 44444444;

} // namespace hogd
