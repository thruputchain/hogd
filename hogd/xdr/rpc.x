// clang-format off

%#include "types.hh"
%#include "contract.hh"
%#include "transaction.hh"

namespace hogd
{
  // clang-format on

  // This is obviously missing error codes.  Please expand or change as you
  // wish.
  // This probably should get moved to a different file?
  enum RPCError {
    kNone = 0,
    // Lookup on contract that does not exist in database
    kContractNExist = 1,
    // Lookup deployment of contract on address that has no deployed contract
    kNoContractDeployed = 2,
    // Executor in bad state to validate or propose
    kBadExecutorState = 3,
  };

  // Is this the XDR pattern we want?
  union ExpectedContract switch (RPCError error)
  {
  case kNone:
    Contract contract;
  default:
    void;
  };

  union ExpectedDeployment switch (RPCError error)
  {
  case kNone:
    SHA256Hash deployed_hash;
  default:
    void;
  };

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

  union ExpectedProposalData switch (RPCError error)
  {
  case kNone:
    NondeterministicProposal proposal_data;
  default:
    void;
  };

  typedef unsigned ExecutorID;
  // First 32 bits of the TransactionID should be the ExecutorID.
  // Each executor can set the lowbits locally.
  typedef unsigned hyper TransactionID;

  // The interface that the executor exposes to the coordinator.
  program ExectutorInterface
  {
    version ExecutorInterfaceV1
    {
      ExpectedProposalData propose_transaction(SignedTransaction) = 0;
      RPCError validate_transaction(ProposedTransaction) = 1;

      // Executor can be in one of three states:
      // None
      // Proposing
      // Validating
      // open_xyz_block() methods set the executor state, or return an error
      // if executor has already opened a block.
      //
      // finish_block() blocks until all running executor threads are finished,
      // and stops the executor from accepting new transactions.
      void open_proposal_block(void) = 2;
      void open_validation_block(void) = 3;

      void finish_block(void) = 4;
    }
    = 1;
  }
  = 22222222;

} // namespace hogd
