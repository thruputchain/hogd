
// clang-format off
%#include "types.hh"
%#include "contract.hh"

namespace hogd
{
  // clang-format on

  typedef opaque Calldata<>;
  typedef opaque ReturnBuffer<>;

  struct Invocation {
    ContractAddress invoked_address;
    unsigned int method;
    GasAmount gas_limit;
    Calldata calldata;
  };

  // Transaction is the object created by the end-user.
  // Signatures should be over this struct.
  //
  // The hash of this object should be unique within a block,
  // as this object should uniquely identify an action on-chain.
  struct Transaction {
    Invocation invocation;
    Contract contracts_to_deploy<>;
  };

  // Deliberately leaving this SignedTransaction object
  // for a later PR.
  //
  // Bundles a transaction with authentication metadata
  // attached by the end user.
  struct SignedTransaction {
    Transaction tx;
  };

  struct LogEntry {
    ContractAddress source_address;
    opaque message<kMaxLogMessageLen>;
  };

  // The nondeterminism attached to a transaction by a
  // block proposer, if any.
  struct NondeterministicProposal {
    opaque try_invoke_results<>;
    unsigned int try_invoke_count;
  };

  struct ProposedTransaction {
    SignedTransaction signed_tx;
    NondeterministicProposal proposal_data;
  };

} // namespace hogd
