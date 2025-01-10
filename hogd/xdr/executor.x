// clang-format off

%#include "types.hh"

namespace hogd
{
  // clang-format on

  // This is obviously missing error codes.  Please expand or change as you
  // wish.
  // This probably should get moved to a different file?
  enum RPCError {
    kNone = 0,
  };

  // Placeholder (this is pending PR 73)
  struct Transaction {
    void;
  };

  struct SignedTransaction {
    Transaction tx;
    // auth/other metadata
  };

  struct NondeterministicProposal {
    void;
    // try_invoke data, and any other nondeterminism in proposer
  };

  struct ProposedTransaction {
    SignedTransaction signed_tx;
    NondeterministicProposal proposal_data;
  };
  // end placeholder

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

  enum ExecutorState { kNone = 0, kProposing = 1, kValidating = 2 };

  // The interface that the executor exposes to the coordinator.
  program ExectutorInterface
  {
    version ExecutorInterfaceV1
    {
      // Error if executor state is not kProposing
      ExpectedProposalData propose_transaction(SignedTransaction) = 0;
      // Error if executor state is not kValidating
      RPCError validate_transaction(ProposedTransaction) = 1;

      // open_xyz_block() methods set the executor state, or return an error
      // if executor has already opened a block.
      void open_proposal_block(void) = 2;
      void open_validation_block(void) = 3;

      // finish_block() blocks until all running executor threads are finished,
      // and stops the executor from accepting new transactions.  Sets
      // executor state to kNone, and clears data cached from database.
      void finish_block(void) = 4;

      // Initialize takes an executor ID, which forms the top bits of the
      // Transaction ID.
      // TODO(geoff): Information about storage server locations
      void initialize(ExecutorID) = 5;
    }
    = 1;
  }
  = 22222222;

} // namespace hogd
