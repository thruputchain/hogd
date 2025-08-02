#include "hogd/mempool/mempool.h"

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

// TODO(jhumphri): When should we remove txs from [vetted_txs_]?

x23::Async<void>
Mempool::schedulercoord_submit_unvetted(SignedTransactionBatch const &batch)
{
  // TODO(jhumphri): Implement vetting logic. For now we just mark all txs as
  // vetted.
  for (const SignedTransaction &tx : batch) {
    vetted_txs_[sha256(tx)] = tx;
  }
  co_return;
}

x23::Async<void>
Mempool::schedulercoord_submit_vetted(ValidatorID validator_id,
                                      SignedTransactionBatch const &batch)
{
  for (const SignedTransaction &tx : batch) {
    vetted_txs_[sha256(tx)] = tx;
  }
  co_return;
}

bool
Mempool::find_vetted_txs(const Nomination &nomination,
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

bool
Mempool::create_snapshot(BlockID prev_block_id, BlockID block_id,
                         BlockExecutionMode mode)
{
  auto res =
      db_client_.dbcoord_create_snapshot(prev_block_id, block_id, mode)();
  return res == DbResult::DbResultOK;
}

void
Mempool::abandon_snapshot(BlockID block_id)
{
  auto res = db_client_.dbcoord_abandon_snapshot(block_id)();
  if (res != DbResult::DbResultOK) {
    // TODO(jhumphri): Any db cleanup needed?
  }
}

bool
Mempool::close_snapshot(BlockID block_id)
{
  auto res = db_client_.dbcoord_close_snapshot(block_id)();
  return res.result() == DbResult::DbResultOK;
}

void
Mempool::finish_block(BlockID block_id)
{
  exec_client_.executorcoord_finish_block(block_id)();
}

// We add each tx hash to the Merkle trie as a key with an empty value. So in
// essence, we are using the Merkle trie as a set. We then compute the root hash
// of the trie and return it.
SHA256Hash
Mempool::get_block_hash(const x23::Vec<SignedTransactionHash> &hashes)
{
  CHECK(!hashes.empty());

  MerkleTriePrefix trie;
  WorkerPool &worker_pool = WorkerPool::get_default_pool();
  size_t num_threads =
      std::min(hashes.size() / kMinHashesPerThread, worker_pool.worker_count());
  if (!num_threads) {
    num_threads = 1;
  }
  CHECK_GT(num_threads, 0UL);
  CHECK_LE(num_threads, worker_pool.worker_count());
  std::vector<std::future<void>> futures;
  futures.reserve(num_threads);

  size_t base = 0;
  size_t chunk_size = hashes.size() / num_threads;
  size_t remainder = hashes.size() % num_threads;
  for (size_t i = 0; i < num_threads; i++) {
    size_t start = base;
    size_t end = base + chunk_size + (i < remainder ? 1 : 0);
    base = end;
    CHECK_GE(start, 0UL);
    if (i == num_threads - 1) {
      CHECK_EQ(end, hashes.size());
    }

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

bool
Mempool::propose_tx(BlockID block_id, const SignedTransaction &tx,
                    x23::XdrVec<NominatedTransaction> &transaction_nominations)
{
  auto res = exec_client_.executorcoord_propose_transaction(block_id, tx)();
  switch (res.result()) {
  case ExecResult::ExecResultTxDeterministicError:
    transaction_nominations.push_back(
        {.nondeterministic_proposal_data = res.proposal_data()});
    return true;
    break;
  case ExecResult::ExecResultOK:
    transaction_nominations.push_back(
        {.nondeterministic_proposal_data = NondeterministicProposalData{}});
    return true;
    break;
  default:
    return false;
    break;
  }
}

bool
Mempool::propose_batch(
    BlockID block_id, const x23::Vec<SignedTransaction> &txs,
    x23::XdrVec<NominatedTransaction> &transaction_nominations)
{
  auto res = exec_client_.executorcoord_propose_batch(block_id, txs)();
  for (const ExecProposeResult &result : res) {
    switch (result.result()) {
    case ExecResult::ExecResultTxDeterministicError:
      transaction_nominations.push_back(
          {.nondeterministic_proposal_data = result.proposal_data()});
      break;
    case ExecResult::ExecResultOK:
      transaction_nominations.push_back(
          {.nondeterministic_proposal_data = NondeterministicProposalData{}});
      break;
    default:
      // TODO(jhumphri): Some of the other txs may have succeeded. Do we need
      // to undo them?
      return false;
      break;
    }
  }
  return true;
}

bool
Mempool::open_proposal_block(BlockID block_id)
{
  auto res = exec_client_.executorcoord_open_proposal_block(block_id)();
  return res == ExecResult::ExecResultOK;
}

x23::Async<SchedulerNominationResult>
Mempool::schedulercoord_nominate(DbTip const &db_tip)
{
  BlockID block_id = get_ns_since_epoch();
  if (!create_snapshot(/*prev_block_id=*/db_tip.id, block_id,
                       BlockExecutionMode::kBlockExecutionModeProposal)) {
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  if (!open_proposal_block(block_id)) {
    abandon_snapshot(block_id);
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  SchedulerNominationResult result(
      SchedulerNominationOutcome::SchedulerNominationOK);
  result.scheduler_nomination().block_id = block_id;
  Nomination &nomination = result.scheduler_nomination().nomination;
  nomination.block_height = db_tip.height + 1;
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
    it++;
  }

  if (txs.size() == 1) {
    if (!propose_tx(block_id, txs[0], nomination.transaction_nominations)) {
      co_return SchedulerNominationResult(
          SchedulerNominationOutcome::SchedulerNominationError);
    }
  }
  else if (txs.size() > 1) {
    if (!propose_batch(block_id, txs, nomination.transaction_nominations)) {
      co_return SchedulerNominationResult(
          SchedulerNominationOutcome::SchedulerNominationError);
    }
  }
  else {
    CHECK(txs.empty());
    // There are no vetted txs, so we cannot propose a block.
    finish_block(block_id);
    abandon_snapshot(block_id);
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  nomination.block_hash = get_block_hash(nomination.transaction_hashes);
  finish_block(block_id);
  if (!close_snapshot(block_id)) {
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  co_return result;
}

bool
Mempool::validate_tx(BlockID block_id, const ProposedTransaction &tx)
{
  auto res = exec_client_.executorcoord_validate_transaction(block_id, tx)();
  if (res != ExecResult::ExecResultOK) {
    finish_block(block_id);
    abandon_snapshot(block_id);
    return false;
  }
  return true;
}

bool
Mempool::validate_batch(BlockID block_id,
                        const x23::Vec<ProposedTransaction> &txs)
{
  auto res = exec_client_.executorcoord_validate_batch(block_id, txs)();
  if (std::any_of(res.begin(), res.end(), [](const auto &r) {
        return r != ExecResult::ExecResultOK;
      })) {
    finish_block(block_id);
    abandon_snapshot(block_id);
    return false;
  }
  return true;
}

bool
Mempool::open_validation_block(BlockID block_id)
{
  auto res = exec_client_.executorcoord_open_validation_block(block_id)();
  return res == ExecResult::ExecResultOK;
}

x23::Async<SchedulerValidationResult>
Mempool::schedulercoord_validate(DbTip const &db_tip,
                                 Nomination const &nomination)
{
  BlockID block_id = get_ns_since_epoch();
  if (!create_snapshot(/*prev_block_id=*/db_tip.id, block_id,
                       BlockExecutionMode::kBlockExecutionModeValidation)) {

    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationDbFailed);
  }

  if (!open_validation_block(block_id)) {
    abandon_snapshot(block_id);
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationExecutorFailed);
  }

  // [nomination] includes the the tx hashes and the nondeterministic proposal
  // data. We need to use these tx hashes to look up the vetted txs in
  // [vetted_txs_]. Assuming we find all of the txs, we then combine them with
  // the nondeterminstic proposal data to form a [ProposedTransaction], which
  // is what the executors expect to receive.
  x23::Vec<ProposedTransaction> txs;
  txs.reserve(nomination.transaction_nominations.size());
  if (!find_vetted_txs(nomination, txs)) {
    finish_block(block_id);
    abandon_snapshot(block_id);
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationMissingTxs);
  }
  if (nomination.transaction_nominations.size() == 1) {
    CHECK_EQ(txs.size(), 1UL);
    if (!validate_tx(block_id, txs[0])) {
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationInvalid);
    }
  }
  else if (nomination.transaction_nominations.size() > 1) {
    CHECK_GT(txs.size(), 1UL);
    if (!validate_batch(block_id, txs)) {
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationInvalid);
    }
  }
  else {
    CHECK(txs.empty());
    // Empty nomination.
    finish_block(block_id);
    abandon_snapshot(block_id);
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

  // Verify the block hash. It is safe to directly use
  // [nomination.transaction_hashes] here because we already checked above that
  // all of these hashes correspond to vetted txs.
  if (get_block_hash(nomination.transaction_hashes) != nomination.block_hash) {
    finish_block(block_id);
    abandon_snapshot(block_id);
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

  finish_block(block_id);
  if (!close_snapshot(block_id)) {
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationDbFailed);
  }

  SchedulerValidationResult result(
      SchedulerValidationOutcome::SchedulerValidationOK);
  result.db_tip() = DbTip{.height = nomination.block_height,
                          .hash = nomination.block_hash,
                          .id = block_id};
  co_return result;
}

x23::Async<void>
Mempool::executorcoord_shutdown()
{
  shutdown_flag_.store(true, std::memory_order_release);
  co_return;
}

} // namespace hogd
