#include "hogd/mempool/rpc_interface.h"

#include <atomic>
#include <cassert>

#include "db_coord.hh"
#include "exec_coord.hh"
#include "hogd/base/types.h"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

x23::Async<void>
MempoolRpcInterface::schedulercoord_submit_unvetted(
    SignedTransactionBatch const &batch)
{
  // TODO(jhumphri): Implement vetting logic. For now we just mark all txs as
  // vetted.
  vetted_txs_.insert(vetted_txs_.end(), batch.cbegin(), batch.cend());
  co_return;
}

x23::Async<void>
MempoolRpcInterface::schedulercoord_submit_vetted(
    ValidatorID validator_id, SignedTransactionBatch const &batch)
{
  vetted_txs_.insert(vetted_txs_.end(), batch.cbegin(), batch.cend());
  co_return;
}

bool
MempoolRpcInterface::find_vetted_txs(
    const x23::Vec<SignedTransactionHash> &hashes,
    std::vector<SignedTransaction> &ret)
{
  // TODO(jhumphri): Make this faster with an [unordered_set].
  ret.clear();
  for (const SignedTransactionHash &hash : hashes) {
    bool found = false;
    for (const SignedTransaction &tx : vetted_txs_) {
      // TODO(jhumphri): Take the hash of [tx].
      if (true) {
        found = true;
        ret.push_back(tx);
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

x23::Async<SchedulerNominationResult>
MempoolRpcInterface::schedulercoord_nominate(DbTip const &db_tip)
{
  BlockID block_id = get_ns_since_epoch();
  // TODO(jhumphri): Choose txs to execute.

  co_return SchedulerNominationResult(
      SchedulerNominationOutcome::SchedulerNominationError);
}

x23::Async<SchedulerValidationResult>
MempoolRpcInterface::schedulercoord_validate(DbTip const &db_tip,
                                             Nomination const &nomination)
{
  BlockID block_id = get_ns_since_epoch();
  auto res = rpc_client_call<DbCoord1, "dbcoord_create_snapshot">(
      *db_client_, db_tip.id, block_id,
      BlockExecutionMode::kBlockExecutionModeValidation);
  if (!res.has_value()) {
    // TODO(jhumphri): Handle error.
  }

  // [nomination] includes the the tx hashes and the nondeterministic proposal
  // data. We need to use these tx hashes to look up the vetted txs in
  // [vetted_txs_]. Assuming we find all of the txs, we then combine them with
  // the nondeterminstic proposal data to form a [ProposedTransaction], which is
  // what the executors expect to receive.
  std::vector<SignedTransaction> txs;
  txs.reserve(nomination.transaction_nominations.size());
  if (!find_vetted_txs(nomination.transaction_hashes, txs)) {
    // TODO(jhumphri): Return error.
  }
  if (nomination.transaction_nominations.size() == 1) {
  }
  else if (nomination.transaction_nominations.size() > 1) {
  }
  else {
    // Empty nomination.
    // TODO(jhumphri): Return error.
  }
  if (!res.has_value()) {
    auto res2 =
        rpc_client_call<DbCoord1, "dbcoord_abandon_snapshot">(*db_client_,
                                                              block_id);
  }

  co_return SchedulerValidationResult(
      SchedulerValidationOutcome::SchedulerValidationInvalid);
}

x23::Async<void>
MempoolRpcInterface::executorcoord_shutdown()
{
  shutdown_flag_.store(true, std::memory_order_release);
  co_return;
}

} // namespace hogd
