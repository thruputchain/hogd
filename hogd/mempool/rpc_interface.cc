#include "hogd/mempool/rpc_interface.h"

#include <atomic>
#include <cassert>

#include "db_coord.hh"
#include "exec_coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/merkle_trie.h"
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

SHA256Hash
MempoolRpcInterface::get_block_hash(
    const x23::Vec<SignedTransactionHash> &hashes)
{
  CHECK(!hashes.empty());

  MerkleTriePrefix trie;
  WorkerPool &worker_pool = WorkerPool::get_default_pool();
  std::vector<std::future<void>> futures;

  size_t num_threads =
      std::min(hashes.size() / kMinHashesPerThread, worker_pool.worker_count());
  if (!num_threads) {
    num_threads = 1;
  }
  // We CHECK at the top of the method that [hashes] is not empty, so
  // [num_threads] must be at least 1. This is important since we divide by
  // [num_threads] below, so we want to make sure we do not divide by 0.
  CHECK_GT(num_threads, 0);
  CHECK_LE(num_threads, worker_pool.worker_count());
  futures.reserve(num_threads);

  size_t base = 0;
  size_t chunk_size = hashes.size() / num_threads;
  size_t remainder = hashes.size() % num_threads;
  for (size_t i = 0; i < num_threads; i++) {
    size_t start = base;
    size_t end = base + chunk_size + (i < remainder ? 1 : 0);
    base = end;

    futures.push_back(worker_pool.submit([start, end, &trie, &hashes] {
      for (size_t j = start; j < end; j++) {
        CHECK(trie.insert(HexArray(hashes[j]), std::span<const std::byte>()));
      }
    }));
  }
  for (std::future<void> &future : futures) {
    future.get();
  }
  trie.generate_top_level_hashes();
  return trie.get_root_hash();
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

  SchedulerNominationResult result(
      SchedulerNominationOutcome::SchedulerNominationOK);
  result.scheduler_nomination().block_id = block_id;
  Nomination &nomination = result.scheduler_nomination().nomination;
  // TODO(jhumphri): What should [block_height] be?
  nomination.block_height = 0;
  nomination.prev_block_hash = db_tip.hash;
  // [nomination.block_hash], [nomination.transaction_hashes], and
  // [nomination.transaction_nominations] are filled in below.
  nomination.transaction_hashes.reserve(kMaxBlockSize);
  nomination.transaction_nominations.reserve(kMaxBlockSize);

  x23::Vec<SignedTransaction> txs;
  txs.reserve(kMaxBlockSize);
  auto it = vetted_txs_.begin();
  for (size_t i = 0; i < kMaxBlockSize && it != vetted_txs_.end(); i++) {
    nomination.transaction_hashes.push_back(it->first);
    txs.push_back(it->second);
    it = vetted_txs_.erase(it);
  }
  nomination.block_hash = get_block_hash(nomination.transaction_hashes);

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
      nomination.transaction_nominations.push_back(
          {.nondeterministic_proposal_data = res.value().proposal_data()});
    }
    else {
      nomination.transaction_nominations.push_back(
          {.nondeterministic_proposal_data = NondeterministicProposalData{}});
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
        nomination.transaction_nominations.push_back(
            {.nondeterministic_proposal_data = result.proposal_data()});
      }
      else {
        nomination.transaction_nominations.push_back(
            {.nondeterministic_proposal_data = NondeterministicProposalData{}});
      }
    }
  }
  else {
    // There are no vetted txs, so we cannot propose a block.
    abandon_snapshot(block_id);
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  {
    auto res = rpc_client_call<DbCoord1, "dbcoord_close_snapshot">(*db_client_,
                                                                   block_id);
    if (!res.has_value() ||
        res.value() != DbBlockResult(DbResult::DbResultOK)) {
      co_return SchedulerNominationResult(
          SchedulerNominationOutcome::SchedulerNominationError);
    }
  }

  co_return result;
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
  // the nondeterminstic proposal data to form a [ProposedTransaction], which
  // is what the executors expect to receive.
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

  // Verify the block hash. It is safe to directly use
  // [nomination.transaction_hashes] here because we already checked above that
  // all of these hashes correspond to vetted txs.
  if (get_block_hash(nomination.transaction_hashes) != nomination.block_hash) {
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

  {
    auto res = rpc_client_call<DbCoord1, "dbcoord_close_snapshot">(*db_client_,
                                                                   block_id);
    if (!res.has_value() ||
        res.value() != DbBlockResult(DbResult::DbResultOK)) {
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationDbFailed);
    }
  }

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
