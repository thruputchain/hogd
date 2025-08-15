
// clang-format off
%#include "types.hh"
%#include "contract.hh"
%#include "retrocausal_queue.hh"

namespace hogd
{
  // clang-format on

  // TXS encoding of the call arguments (as a tuple)
  typedef opaque Calldata<>;

  typedef opaque ReturnBuffer<>;

  struct Invocation {
    ContractAddress invoked_address;
    ContractEntrypointID entrypoint;
    GasAmount gas_limit;
    Calldata calldata;
  };

  enum InvokeFlag {
    kInvokeFlagNone = 0,
    kInvokeFlagTryInvoke = 0x00000001,
    kInvokeFlagReadOnly = 0x00000002,
    kInvokeFlagDelegateCall = 0x00000004,
  };

  typedef int InvokeFlagSet;

  typedef opaque OriginID[16];

  struct Transaction {
    OriginID origin_id;
    Timestamp created;
    Invocation invocation;
    Contract contracts_to_deploy<>;
  };

  typedef SHA256Hash SignedTransactionHash;

  // Deliberately leaving this SignedTransaction object
  // (and all the other metadata)
  // for a later PR.
  struct SignedTransaction {
    Signature signature;
    Transaction tx;
  };

  // Nondeterministic data attached to a transaction by the proposer.
  struct NondeterministicProposalData {
    // string of nondeterministic bits generated during the course
    // of the transaction.
    opaque nondeterministic_bits<>;
    // the length of the nondeterministic_bits string, in bits
    // (nondeterminstic_bits.size() returns the rounded-up length in bytes)
    unsigned int nondeterministic_bit_count;
    // sequence of queue entries observed by the proposer
    QueueEntry observed_queue_entry_sequence<>;
  };

  // A signed transaction, with the nondeterministic data from the proposer
  // attached to it.
  // TODO(petar): Who is the signer? The originator of the transaction or the
  // validator proposing it?
  // TODO(petar): Should the signature capture the non-deterministic data?
  struct ProposedTransaction {
    SignedTransaction signed_tx;
    NondeterministicProposalData nondeterministic_proposal_data;
  };

  // Nominations, included in SCP messages, include only the ND data.
  // The transactions themselves are disseminated through the overlay.
  struct NominatedTransaction {
    NondeterministicProposalData nondeterministic_proposal_data;
  };

  enum TransactionStatistic {
    kTxStatGasRemaining = 0,
  };

} // namespace hogd
