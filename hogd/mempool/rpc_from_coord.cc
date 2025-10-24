#include "hogd/mempool/rpc_from_coord.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <future>
#include <memory>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "block.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "exec_sched.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mempool/authentication.h"
#include "nomination.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/vec.h"

namespace hogd {

// TODO(jhumphri): When should we remove txs from [vetted_txs_]?

x23::Async<void>
MempoolCoordinatorRpc::schedulercoord_submit_unvetted(
    SignedTransactionBatch batch)
{
  // TODO(jhumphri): Is any other vetting logic needed?
  SignedTransactionBatch authenticated;
  authenticated.reserve(batch.size());
  for (const SignedTransaction &tx : batch) {
    if (co_await authentication_->authenticate(tx)) {
      authenticated.push_back(tx);
    }
  }
  x23::CoroGuard cg;
  co_await cg(scheduler_->global_lock_);
  co_await scheduler_->add_txs(authenticated);
  cg.reset();
  co_return co_await coord_client_->coordscheduler_notify_vetted(authenticated);
}

x23::Async<void>
MempoolCoordinatorRpc::schedulercoord_submit_vetted(
    ValidatorID validator_id, SignedTransactionBatch batch)
{
  x23::CoroGuard cg;
  co_await cg(scheduler_->global_lock_);
  co_await scheduler_->add_txs(batch);
  cg.reset();
  co_return;
}

x23::Async<bool>
MempoolCoordinatorRpc::create_snapshot(BlockID prev_block_id, BlockID block_id,
                                       BlockExecutionMode mode)
{
  DbSchedResult res =
      co_await db_sched_client_->dbsched_create_snapshot(prev_block_id,
                                                         block_id, mode);
  if (res != DbSchedResult::DbSchedResultOK) {
    co_return false;
  }
  co_await event_sched_client_->eventsched_open_block(block_id);
  co_return true;
}

x23::Async<void>
MempoolCoordinatorRpc::abandon_snapshot(BlockID block_id)
{
  DbSchedResult res =
      co_await db_sched_client_->dbsched_abandon_snapshot(block_id);
  // TODO(jhumphri): Geoff currently always returns [DbSchedResultOK], but this
  // could change in the future. In the case of a failure, we need to clean up
  // db state.
  CHECK_EQ(res, DbSchedResult::DbSchedResultOK);
  co_await event_sched_client_->eventsched_abandon_block(block_id);
  co_return;
}

x23::Async<void>
MempoolCoordinatorRpc::close_snapshot(BlockID block_id)
{
  DbBlockResult res =
      co_await db_sched_client_->dbsched_close_snapshot(block_id);
  // [dbsched_close_snapshot] will always succeed as long as we provide a valid
  // block ID.
  CHECK_EQ(res.result(), DbSchedResult::DbSchedResultOK);
  auto r = co_await event_sched_client_->eventsched_close_block(block_id);
  CHECK_EQ(r.error(), RPCError::kRPCNone);
  // TODO(geoff): forward the root hash of the event block back to the block
  // header
  // TODO(geoff): consider whether this should be called from commit instead
  co_return;
}

x23::Async<void>
MempoolCoordinatorRpc::commit_snapshot(BlockID block_id)
{
  DbBlockResult res =
      co_await db_sched_client_->dbsched_commit_snapshot(block_id);
  // [dbsched_commit_snapshot] will always succeed as long as we provide a valid
  // block ID.
  CHECK_EQ(res.result(), DbSchedResult::DbSchedResultOK);
  co_return;
}

x23::Async<void>
MempoolCoordinatorRpc::finish_block(BlockID block_id)
{
  return exec_clients_[0].executorsched_finish_block(block_id);
}

// We add each tx hash to the Merkle trie as a key with an empty value. So in
// essence, we are using the Merkle trie as a set. We then compute the root hash
// of the trie and return it.
SHA256Hash
MempoolCoordinatorRpc::get_block_hash(
    const x23::Vec<SignedTransactionHash> &hashes,
    const BlockHash &prev_block_hash)
{
  CHECK(!hashes.empty());

  MerkleTriePrefix trie;
  WorkerPool &worker_pool = WorkerPool::get_default_pool();
  // The optimal number of threads to use varies with the number of hashes we
  // are adding to the trie. For example, if we are adding 128 hashes to the
  // trie, it is probably slower to use 128 threads rather than 1-2 threads due
  // to lock contention.
  size_t num_threads = worker_pool.worker_count();
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
  SHA256Hash root_hash = trie.get_root_hash();

  BlockHashPreimage preimage{.txs_merkle_trie_root_hash = root_hash,
                             .prev_block_hash = prev_block_hash};
  return sha256(preimage);
}

x23::Async<x23::Vec<bool>>
MempoolCoordinatorRpc::propose_batch(
    BlockID block_id, const x23::Vec<SignedTransaction> &txs,
    x23::XdrVec<NominatedTransaction> &transaction_nominations)
{
  x23::Vec<bool> ret;
  ret.reserve(txs.size());

  ExecProposeBatchResult res =
      co_await exec_clients_[0].executorsched_propose_batch(block_id, txs);
  for (const ExecProposeResult &result : res) {
    switch (result.result()) {
    case ExecResult::ExecResultTxDeterministicError:
      transaction_nominations.push_back(
          {.nondeterministic_proposal_data = result.proposal_data()});
      ret.push_back(true);
      break;
    case ExecResult::ExecResultOK:
      transaction_nominations.push_back(
          {.nondeterministic_proposal_data = NondeterministicProposalData{}});
      ret.push_back(true);
      break;
    default:
      ret.push_back(false);
      break;
    }
  }
  co_return ret;
}

x23::Async<x23::Vec<bool>>
MempoolCoordinatorRpc::propose_batch_in_chunks(
    BlockID block_id, const x23::Vec<SignedTransaction> &txs,
    x23::XdrVec<NominatedTransaction> &transaction_nominations,
    size_t chunk_size)
{
  CHECK_GT(chunk_size, 0UL);

  // Create the chunks.
  size_t num_chunks =
      (txs.size() / chunk_size) + ((txs.size() % chunk_size) ? 1 : 0);
  x23::Vec<x23::Vec<SignedTransaction>> chunks;
  chunks.reserve(num_chunks);
  for (size_t i = 0; i < num_chunks; i++) {
    auto end =
        (i < num_chunks - 1) ? txs.begin() + ((i + 1) * kChunkSize) : txs.end();
    chunks.emplace_back(txs.begin() + (i * kChunkSize), end);
  }

  // Send the RPCs.
  x23::Vec<x23::Async<x23::Vec<bool>>> async_handles;
  x23::Vec<x23::XdrVec<NominatedTransaction>> nomination_chunks(num_chunks, {});
  async_handles.reserve(num_chunks);
  for (size_t i = 0; i < num_chunks; i++) {
    async_handles.push_back(
        propose_batch(block_id, chunks[i], nomination_chunks[i]));
  }

  // Wait for the RPC responses.
  x23::Vec<x23::Vec<bool>> ret_chunks(num_chunks, {});
  for (size_t i = 0; i < num_chunks; i++) {
    x23::Vec<bool> ret_chunk = co_await async_handles[i];
    ret_chunks[i] = ret_chunk;
  }

  // Fold the results.
  x23::Vec<bool> ret;
  ret.reserve(txs.size());
  transaction_nominations.reserve(txs.size());
  for (size_t i = 0; i < num_chunks; i++) {
    for (bool r : ret_chunks[i]) {
      ret.push_back(r);
    }
    for (NominatedTransaction &nominated_tx : nomination_chunks[i]) {
      transaction_nominations.push_back(nominated_tx);
    }
  }

  co_return ret;
}

x23::Async<bool>
MempoolCoordinatorRpc::open_proposal_block(BlockID block_id)
{
  ExecResult res =
      co_await exec_clients_[0].executorsched_open_proposal_block(block_id);
  co_return res == ExecResult::ExecResultOK;
}

x23::Async<SchedulerNominationResult>
MempoolCoordinatorRpc::schedulercoord_nominate(DbTip db_tip)
{
  // TODO(jhumphri): Replace with [CoroSem] when that is merged into main.
  auto timer = std::make_unique<ElapsedTimer>();
  while (!scheduler_->transfer_finished_) {
    co_await default_coropoll().pool_.bg();
  }
  scheduler_->transfer_finished_ = false;

  double wait_for_transfer_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();
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
  double open_block_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

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
      co_await scheduler_->schedule(/*block_height=*/nomination.block_height,
                                    kMaxBlockSize);
  double schedule_duration = timer->elapsed();

  info("schedulercoord_nominate(), scheduler gave us {} txs, {} are runnable, "
       "{} are stranded",
       txs.size(), scheduler_->num_on_runqueue(), scheduler_->num_stranded());
  if (nomination.transaction_hashes.empty()) {
    // There are no vetted txs, so we cannot propose a block.
    co_await finish_block(block_id);
    co_await abandon_snapshot(block_id);
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  timer = std::make_unique<ElapsedTimer>();
  x23::Vec<bool> statuses =
      co_await propose_batch_in_chunks(block_id, txs,
                                       nomination.transaction_nominations,
                                       kChunkSize);
  double run_on_executor_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  std::unordered_map<ContractAddress, x23::Vec<SignedTransactionHash>>
      tx_hashes;
  // We need to track the failed txs themselves (not just the hashes) because we
  // re-enqueue these txs in the scheduler.
  std::unordered_map<ContractAddress, SignedTransactionBatch> failed_txs;
  std::unordered_set<ContractAddress> all_accounts;
  // We remove failed txs from [nomination.transaction_hashes], so we iterate
  // backwards.
  for (ssize_t i = statuses.size() - 1; i >= 0; i--) {
    const ContractAddress &address = txs[i].tx.invocation.invoked_address;
    all_accounts.insert(address);
    tx_hashes[address].push_back(nomination.transaction_hashes[i]);
    if (!statuses[i]) {
      failed_txs[address].push_back(txs[i]);
    }
  }
  for (const ContractAddress &account : all_accounts) {
    co_await scheduler_->mark_txs_finished(tx_hashes[account]);
    if (!failed_txs[account].empty()) {
      co_await scheduler_->add_txs(failed_txs[account]);
    }
  }

  if (nomination.transaction_hashes.empty()) {
    // It is possible that all txs failed to execute, in which case the block
    // is empty. Thus, we fail the nomination.
    co_await finish_block(block_id);
    co_await abandon_snapshot(block_id);
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  double mark_txs_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  x23::Async<void> finish_block_handle = finish_block(block_id);
  x23::Async<void> close_snapshot_handle = close_snapshot(block_id);
  x23::Async<void> commit_snapshot_handle = commit_snapshot(block_id);
  x23::Async<void> mark_block_committed_handle = mark_block_committed(block_id);

  double create_handles_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  nomination.block_hash = get_block_hash(nomination.transaction_hashes,
                                         /*prev_block_hash=*/db_tip.hash);

  double get_block_hash_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  co_await finish_block_handle;
  co_await close_snapshot_handle;
  co_await commit_snapshot_handle;
  co_await mark_block_committed_handle;

  double wait_for_handles_duration = timer->elapsed();

  info("wait_for_transfer_duration {}, open_block_duration {}, "
       "schedule_duration {}, run_on_executor_duration {}, mark_txs_duration "
       "{}, create_handles_duration {}, get_block_hash_duration {}, "
       "wait_for_handles_duration {}",
       wait_for_transfer_duration, open_block_duration, schedule_duration,
       run_on_executor_duration, mark_txs_duration, create_handles_duration,
       get_block_hash_duration, wait_for_handles_duration);

  co_return result;
}

x23::Async<bool>
MempoolCoordinatorRpc::validate_batch(BlockID block_id,
                                      const x23::Vec<ProposedTransaction> &txs)
{
  ExecValidateBatchResult res =
      co_await exec_clients_[0].executorsched_validate_batch(block_id, txs);
  if (std::ranges::any_of(res, [](const ExecResult &r) {
        return r != ExecResult::ExecResultOK;
      })) {
    co_await finish_block(block_id);
    co_await abandon_snapshot(block_id);
    co_return false;
  }
  co_return true;
}

x23::Async<bool>
MempoolCoordinatorRpc::open_validation_block(BlockID block_id)
{
  ExecResult res =
      co_await exec_clients_[0].executorsched_open_validation_block(block_id);
  co_return res == ExecResult::ExecResultOK;
}

x23::Async<SchedulerValidationResult>
MempoolCoordinatorRpc::schedulercoord_validate(DbTip db_tip,
                                               Nomination nomination)
{
  if (db_tip.height + 1 != nomination.block_height) {
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }
  if (db_tip.hash != nomination.prev_block_hash) {
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

  // TODO(jhumphri): Need to ensure that the node that proposed the block is
  // following the expected scheduling policy.
  if (nomination.transaction_nominations.empty()) {
    // Empty nomination.
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

  while (!scheduler_->transfer_finished_) {
    co_await default_coropoll().pool_.bg();
  }
  scheduler_->transfer_finished_ = false;

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
  x23::Vec<SignedTransaction> signed_txs;
  {
    if (!scheduler_->find_and_remove_from_runqueue(
            nomination.transaction_hashes, &signed_txs)) {
      co_await finish_block(block_id);
      co_await abandon_snapshot(block_id);
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationMissingTxs);
    }
    CHECK_EQ(signed_txs.size(), nomination.transaction_nominations.size());
    for (size_t i = 0; i < signed_txs.size(); i++) {
      txs.push_back({.signed_tx = signed_txs[i],
                     .nondeterministic_proposal_data =
                         nomination.transaction_nominations[i]
                             .nondeterministic_proposal_data});
    }
  }
  CHECK(!txs.empty());

  // Now execute the txs on the executor.
  bool validated = co_await validate_batch(block_id, txs);
  co_await scheduler_->mark_txs_finished(nomination.transaction_hashes);

  // Check [validated] and verify the block hash. It is safe to directly use
  // [nomination.transaction_hashes] here because we already checked above
  // that all of these hashes correspond to vetted txs.
  if (!validated || get_block_hash(nomination.transaction_hashes,
                                   /*prev_block_hash=*/db_tip.hash) !=
                        nomination.block_hash) {
    co_await scheduler_->add_txs(signed_txs);

    co_await finish_block(block_id);
    co_await abandon_snapshot(block_id);
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

  co_await finish_block(block_id);
  co_await close_snapshot(block_id);
  co_await commit_snapshot(block_id);
  co_await mark_block_committed(block_id);

  SchedulerValidationResult result(
      SchedulerValidationOutcome::SchedulerValidationOK);
  result.db_tip() = DbTip{.height = nomination.block_height,
                          .hash = nomination.block_hash,
                          .id = block_id};

  co_return result;
}

x23::Async<void>
MempoolCoordinatorRpc::mark_block_committed(BlockID block_id)
{
  current_block_id_ = block_id;
  co_return;

  // TODO(jhumphri): We are temporarily disabling authentication until it is
  // implemented in hogexec.
  // x23::Vec<ContractAddress> changed_accounts =
  //     co_await authentication_->update_block_id(current_block_id_);
  // for (const ContractAddress &account : changed_accounts) {
  //   co_await scheduler_->filter_txs_for_account(account, auth_filter_);
  // }
}

x23::Async<DbTipResult>
MempoolCoordinatorRpc::schedcoord_get_tip()
{
  return db_sched_client_->dbsched_get_tip();
}

x23::Async<void>
MempoolCoordinatorRpc::schedulercoord_shutdown()
{
  shutdown_flag_.store(true, std::memory_order_release);
  co_return;
}

} // namespace hogd
