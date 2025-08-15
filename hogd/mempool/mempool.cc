#include "hogd/mempool/mempool.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <future>
#include <span>
#include <vector>

#include "db_coord.hh"
#include "discovery.hh"
#include "exec_coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "nomination.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

// TODO(jhumphri): When should we remove txs from [vetted_txs_]?

x23::Async<void>
Mempool::schedulercoord_submit_unvetted(SignedTransactionBatch batch)
{
  // TODO(jhumphri): Implement vetting logic. For now we just mark all txs as
  // vetted. This vetting logic will include the authentication logic from
  // Geoff's proposal.
  CHECK(scheduler_.add_vetted_txs(batch));
  co_return co_await coord_client_.coordscheduler_notify_vetted(batch);
}

x23::Async<void>
Mempool::schedulercoord_submit_vetted(ValidatorID validator_id,
                                      SignedTransactionBatch batch)
{
  CHECK(scheduler_.add_vetted_txs(batch));
  co_return;
}

x23::Async<bool>
Mempool::create_snapshot(BlockID prev_block_id, BlockID block_id,
                         BlockExecutionMode mode)
{
  DbResult res = co_await db_client_.dbcoord_create_snapshot(prev_block_id,
                                                             block_id, mode);
  co_return res == DbResult::DbResultOK;
}

x23::Async<void>
Mempool::abandon_snapshot(BlockID block_id)
{
  DbResult res = co_await db_client_.dbcoord_abandon_snapshot(block_id);
  // TODO(jhumphri): Geoff currently always returns [DbResultOK], but this could
  // change in the future. In the case of a failure, we need to clean up db
  // state.
  CHECK_EQ(res, DbResult::DbResultOK);
  co_return;
}

x23::Async<void>
Mempool::close_snapshot(BlockID block_id)
{
  DbBlockResult res = co_await db_client_.dbcoord_close_snapshot(block_id);
  // [dbcoord_close_snapshot] will always succeed as long as we provide a valid
  // block ID.
  CHECK_EQ(res.result(), DbResult::DbResultOK);
  co_return;
}

x23::Async<void>
Mempool::finish_block(BlockID block_id)
{
  return exec_client_.executorcoord_finish_block(block_id);
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
  // The optimal number of threads to use varies with the number of hashes we
  // are adding to the trie. For example, if we are adding 128 hashes to the
  // trie, it is probably slower to use 128 threads rather than 1-2 threads due
  // to lock contention.
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

x23::Async<bool>
Mempool::propose_batch(
    BlockID block_id, const x23::Vec<SignedTransaction> &txs,
    x23::XdrVec<NominatedTransaction> &transaction_nominations)
{
  ExecProposeBatchResult res =
      co_await exec_client_.executorcoord_propose_batch(block_id, txs);
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
      co_return false;
      break;
    }
  }
  co_return true;
}

x23::Async<bool>
Mempool::open_proposal_block(BlockID block_id)
{
  ExecResult res =
      co_await exec_client_.executorcoord_open_proposal_block(block_id);
  co_return res == ExecResult::ExecResultOK;
}

x23::Async<SchedulerNominationResult>
Mempool::schedulercoord_nominate(DbTip db_tip)
{
  BlockID block_id = get_ns_since_epoch();
  if (!co_await create_snapshot(
          /*prev_block_id=*/db_tip.id, block_id,
          BlockExecutionMode::kBlockExecutionModeProposal)) {
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  if (!co_await open_proposal_block(block_id)) {
    co_await abandon_snapshot(block_id);
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
  std::tie(nomination.transaction_hashes, txs) =
      scheduler_.schedule(kMaxBlockSize);
  if (nomination.transaction_hashes.empty()) {
    // There are no vetted txs, so we cannot propose a block.
    co_await finish_block(block_id);
    co_await abandon_snapshot(block_id);
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }
  else if (!co_await propose_batch(block_id, txs,
                                   nomination.transaction_nominations)) {
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  nomination.block_hash = get_block_hash(nomination.transaction_hashes);
  co_await finish_block(block_id);
  co_await close_snapshot(block_id);

  co_return result;
}

x23::Async<bool>
Mempool::validate_batch(BlockID block_id,
                        const x23::Vec<ProposedTransaction> &txs)
{
  ExecValidateBatchResult res =
      co_await exec_client_.executorcoord_validate_batch(block_id, txs);
   printf("it is %s\n", HexArray(sha256(txs[0])).str().c_str());
  if (std::ranges::any_of(res, [](const ExecResult &r) {
        printf("r is %d\n", r);
        return r != ExecResult::ExecResultOK;
      })) {
    co_await finish_block(block_id);
    co_await abandon_snapshot(block_id);
    co_return false;
  }
  co_return true;
}

x23::Async<bool>
Mempool::open_validation_block(BlockID block_id)
{
  ExecResult res =
      co_await exec_client_.executorcoord_open_validation_block(block_id);
  co_return res == ExecResult::ExecResultOK;
}

x23::Async<SchedulerValidationResult>
Mempool::schedulercoord_validate(DbTip db_tip, Nomination nomination)
{
  // TODO(jhumphri): Need to ensure that the node that proposed the block is
  // following the expected scheduling policy.

  BlockID block_id = get_ns_since_epoch();
  if (!co_await create_snapshot(
          /*prev_block_id=*/db_tip.id, block_id,
          BlockExecutionMode::kBlockExecutionModeValidation)) {
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationDbFailed);
  }

  if (!co_await open_validation_block(block_id)) {
    co_await abandon_snapshot(block_id);
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationExecutorFailed);
  }

  // [nomination] includes the the tx hashes and the nondeterministic proposal
  // data. We need to use these tx hashes to look up the vetted txs in
  // [vetted_txs_]. Assuming we find all of the txs, we then combine them with
  // the nondeterminstic proposal data to form a [ProposedTransaction], which
  // is what the executors expect to receive.
  x23::Vec<ProposedTransaction> txs;
  if (!scheduler_.find_and_remove_vetted_txs(nomination, txs)) {
    co_await finish_block(block_id);
    co_await abandon_snapshot(block_id);
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationMissingTxs);
  }
  if (nomination.transaction_nominations.empty()) {
    CHECK(txs.empty());
    // Empty nomination.
    co_await finish_block(block_id);
    co_await abandon_snapshot(block_id);
    printf("A\n");
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }
  else {
    CHECK(!txs.empty());
    if (!co_await validate_batch(block_id, txs)) {
      printf("B\n");
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationInvalid);
    }
  }

  // Verify the block hash. It is safe to directly use
  // [nomination.transaction_hashes] here because we already checked above that
  // all of these hashes correspond to vetted txs.
  if (get_block_hash(nomination.transaction_hashes) != nomination.block_hash) {
    co_await finish_block(block_id);
    co_await abandon_snapshot(block_id);
    printf("C\n");
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

  co_await finish_block(block_id);
  co_await close_snapshot(block_id);

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
