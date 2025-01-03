
// clang-format off
%#include "types.hh"
%#include "contract.hh"

namespace hogd
{
  // clang-format on

  // Transaction is the object created by the end-user.
  // Signatures should be over this struct.
  // Will specify a method invocation, and any other
  // invocation parameters.
  //
  // The hash of this object should be unique within a block,
  // as this object should uniquely identify an action on-chain.
  struct Transaction {
    void;
  };

  // Bundles a transaction with authentication metadata
  // attached by the end user.
  struct SignedTransaction {
    Transaction tx;
    // Auth data goes here
  };

  // The nondeterminism attached to a transaction by a 
  // block proposer, if any.
  struct NondeterministicProposal {
    void;
  };

  struct ProposedTransaction {
    SignedTransaction signed_tx;
    NondeterministicProposal proposal_data;
  };

} // namespace hogd
