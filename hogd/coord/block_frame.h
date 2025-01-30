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

  BlockValidationFrame(BlockValidationFrame const &) = delete;
  BlockValidationFrame &operator=(BlockValidationFrame const &) = delete;
  BlockValidationFrame(BlockValidationFrame &&) = delete;
  BlockValidationFrame &operator=(BlockValidationFrame &&) = delete;

  // returns nullptr if all transactions are validated.
  // The intended workflow would be of the form
  //
  // while (auto* next_tx = frame.get_next_validation_transaction()) {
  //   if (!validate(next_tx)) return false;
  // }
  // return true;
  //
  const ProposedTransaction *get_next_validation_transaction();

  bool all_txs_queried()
  {
    return validation_index_.load(std::memory_order_relaxed) ==
           transactions_.size();
  }

  void mark_validation_failed() { validation_failed_ = true; }
  bool validation_failed() const { return validation_failed_; }
  void mark_validation_finished() { validation_finished_ = true; }

  bool validation_success() const
  {
    return validation_finished_ && !validation_failed_;
  }

private:
  const std::vector<ProposedTransaction> transactions_;
  std::atomic<uint32_t> validation_index_ = 0;
  std::atomic<bool> validation_failed_ = false;
  bool validation_finished_ = false;
};

} // namespace hogd
