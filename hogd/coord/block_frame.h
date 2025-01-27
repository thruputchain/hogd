#pragma once

#include <atomic>
#include <map>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/mvar.h"

namespace hogd {

// Responsible for managing a set of transactions to be gathered into a block
// proposal.
//
// A ProposedTransaction is identified by the hash of the inner Transaction
// object. The NondeterministicProposal component of ProposedTransaction, as
// well as the contents of SignedTransaction \ Transaction, are treated as
// data augmenting the Transaction (the difference being that they come from
// different sources).
//
// A block may only contain one copy of each Transaction (regardless of
// signatures/proposal data).
class BlockProposalFrame {
public:
  // Return true only if tx_hash = sha256(signed_tx.tx) is not already present
  // as a transaction in this proposal.
  // If returns true, then tx_hash is added to the set of transactions.
  bool try_add_transaction(SignedTransaction const &signed_tx);

  // After calling try_add_transaction, must either call
  // cancel_add_transaction or finish_add_transaction.
  void cancel_add_transaction(SHA256Hash const &tx_hash);
  void finish_add_transaction(SHA256Hash const &tx_hash,
                              NondeterministicProposal const &proposal);

  // TODO(geoff): we need to decide on how to canonically hash a block.
  SHA256Hash canonical_block_hash() const;

  // TODO(geoff): This serialization will depend on how the overlay works,
  // and how blocks are transmitted.  For now, I leave in a simple to_list()
  // to facilitate local tests,.
  std::vector<ProposedTransaction> to_list();

private:
  // Not a concurrent implementation.  Internal data structures
  // will be replaced as necessary.
  x23::MVar<std::map<SHA256Hash, ProposedTransaction>> transactions_;
};

// Manage a set of transactions to validate. Threads shouuld call
// get_next_validation_transation() until that method returns nullptr, or one
// transaction fails to validate.
//
// TODO(geoff): If contention on
// validation_index_ becomes a problem, we can easily swap to a batched
// interface (i.e. get_next_10_transactions or the like).
class BlockValidationFrame {
public:
  BlockValidationFrame(std::vector<ProposedTransaction> txs)
    : transactions_(std::move(txs))
  {}

  // returns nullptr if all transactions are validated.
  // The intended workflow would be of the form
  //
  // while (auto* next_tx = frame.get_next_validation_transaction()) {
  //   if (!validate(next_tx)) return false;
  // }
  // return true;
  //
  const ProposedTransaction *get_next_validation_transaction();

private:
  const std::vector<ProposedTransaction> transactions_;
  std::atomic<uint32_t> validation_index_ = 0;
};

} // namespace hogd
