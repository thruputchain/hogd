#include "hogd/mempool/rpc_interface.h"

#include <atomic>
#include <cassert>

#include "db_coord.hh"
#include "exec_coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
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
  for (const SignedTransaction &tx : batch) {
    vetted_txs_[sha256(tx)] = tx;
  }
  co_return;
}

x23::Async<void>
MempoolRpcInterface::schedulercoord_submit_vetted(
    ValidatorID validator_id, SignedTransactionBatch const &batch)
{
  for (const SignedTransaction &tx : batch) {
    vetted_txs_[sha256(tx)] = tx;
  }
  co_return;
}

bool
MempoolRpcInterface::find_vetted_txs(const Nomination &nomination,
                                     x23::Vec<ProposedTransaction> &ret)
{
  CHECK_EQ(nomination.transaction_hashes.size(),
           nomination.transaction_nominations.size());

  ret.clear();
  for (size_t i = 0; i < nomination.transaction_hashes.size(); i++) {
    const SignedTransactionHash &hash = nomination.transaction_hashes[i];
    const NominatedTransaction &nominated_tx =
        nomination.transaction_nominations[i];

    auto it = vetted_txs_.find(hash);
    if (it == vetted_txs_.end()) {
      return false;
    }
    ret.push_back({.signed_tx = it->second,
                   .nondeterministic_proposal_data =
                       nominated_tx.nondeterministic_proposal_data});
  }
  return true;
}

void
MempoolRpcInterface::abandon_snapshot(BlockID block_id)
{
  auto res = rpc_client_call<DbCoord1, "dbcoord_abandon_snapshot">(*db_client_,
                                                                   block_id);
  if (!res.has_value() || res != DbResult::DbResultOK) {
    // TODO(jhumphri): Any db cleanup needed?
  }
}

x23::Async<SchedulerNominationResult>
MempoolRpcInterface::schedulercoord_nominate(DbTip const &db_tip)
{
  BlockID block_id = get_ns_since_epoch();
  {
    auto res = rpc_client_call<DbCoord1, "dbcoord_create_snapshot">(
        *db_client_, db_tip.id, block_id,
        BlockExecutionMode::kBlockExecutionModeProposal);
    if (!res.has_value() || res.value() != DbResult::DbResultOK) {
      co_return SchedulerNominationResult(
          SchedulerNominationOutcome::SchedulerNominationError);
    }
  }

  x23::Vec<SignedTransaction> txs;
  auto it = vetted_txs_.begin();
  for (size_t i = 0; i < kMaxBlockSize && it != vetted_txs_.end(); i++) {
    txs.push_back(it->second);
    it = vetted_txs_.erase(it);
  }

  if (txs.size() == 1) {
    auto res =
        rpc_client_call<ExecutorCoord1, "executorcoord_propose_transaction">(
            *exec_client_, block_id, txs[0]);
    if (!res.has_value()) {
      abandon_snapshot(block_id);
      co_return SchedulerNominationResult(
          SchedulerNominationOutcome::SchedulerNominationError);
    }
    if (res.value() ==
        ExecProposeResult(ExecResult::ExecResultTxDeterministicError)) {
      // TODO(jhumphri): Handle the nondeterministic data.
    }
  }
  else if (txs.size() > 1) {
    auto res = rpc_client_call<ExecutorCoord1, "executorcoord_propose_batch">(
        *exec_client_, block_id, txs);
    if (!res.has_value()) {
      abandon_snapshot(block_id);
      co_return SchedulerNominationResult(
          SchedulerNominationOutcome::SchedulerNominationError);
    }
    for (const ExecProposeResult &result : res.value()) {
      if (result ==
          ExecProposeResult(ExecResult::ExecResultTxDeterministicError)) {
        // TODO(jhumphri): Handle the nondeterministic data.
      }
    }
  }
  else {
    // There are no vetted txs, so we cannot propose a block.
    abandon_snapshot(block_id);
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  // TODO(jhumphri): Close snapshot in db.

  co_return SchedulerNominationResult(
      SchedulerNominationOutcome::SchedulerNominationError);
}

x23::Async<SchedulerValidationResult>
MempoolRpcInterface::schedulercoord_validate(DbTip const &db_tip,
                                             Nomination const &nomination)
{
  BlockID block_id = get_ns_since_epoch();
  {
    auto res = rpc_client_call<DbCoord1, "dbcoord_create_snapshot">(
        *db_client_, db_tip.id, block_id,
        BlockExecutionMode::kBlockExecutionModeValidation);
    if (!res.has_value() || res.value() != DbResult::DbResultOK) {
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationDbFailed);
    }
  }

  // [nomination] includes the the tx hashes and the nondeterministic proposal
  // data. We need to use these tx hashes to look up the vetted txs in
  // [vetted_txs_]. Assuming we find all of the txs, we then combine them with
  // the nondeterminstic proposal data to form a [ProposedTransaction], which is
  // what the executors expect to receive.
  x23::Vec<ProposedTransaction> txs;
  txs.reserve(nomination.transaction_nominations.size());
  if (!find_vetted_txs(nomination, txs)) {
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationMissingTxs);
  }
  if (nomination.transaction_nominations.size() == 1) {
    CHECK_EQ(txs.size(), 1);
    auto res =
        rpc_client_call<ExecutorCoord1, "executorcoord_validate_transaction">(
            *exec_client_, block_id, txs[0]);
    if (!res.has_value() || res.value() != ExecResult::ExecResultOK) {
      abandon_snapshot(block_id);
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationInvalid);
    }
  }
  else if (nomination.transaction_nominations.size() > 1) {
    CHECK_GT(txs.size(), 1);
    auto res = rpc_client_call<ExecutorCoord1, "executorcoord_validate_batch">(
        *exec_client_, block_id, txs);
    if (!res.has_value() ||
        std::any_of(res.value().begin(), res.value().end(), [](const auto &r) {
          return r != ExecResult::ExecResultOK;
        })) {
      abandon_snapshot(block_id);
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationInvalid);
    }
  }
  else {
    // Empty nomination.
    CHECK(txs.empty());
    abandon_snapshot(block_id);
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

  // TODO(jhumphri): Verify block hash.
  // TODO(jhumphri): Close snapshot in db.

  SchedulerValidationResult result(
      SchedulerValidationOutcome::SchedulerValidationOK);
  result.db_tip() = DbTip{.height = nomination.block_height,
                          .hash = nomination.block_hash,
                          .id = block_id};
  co_return result;
}

x23::Async<void>
MempoolRpcInterface::executorcoord_shutdown()
{
  shutdown_flag_.store(true, std::memory_order_release);
  co_return;
}

} // namespace hogd
