#include "hogd/mempool/rpc_from_coord.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <future>
#include <memory>
#include <span>
#include <tuple>
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
  auto timer = std::make_unique<ElapsedTimer>();

  x23::CoroGuard cg;
  info("cg acquire start");
  co_await cg(scheduler_->global_lock_);

  double get_lock_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  co_await scheduler_->add_txs(batch);
  cg.reset();
  info("cg acquire end");

  double add_txs_duration = timer->elapsed();
  info("mempool schedulercoord_submit_vetted get_lock_duration {}, "
       "add_txs_duration {}",
       get_lock_duration, add_txs_duration);

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
MempoolCoordinatorRpc::finish_block(BlockID block_id, size_t executor_idx)
{
  return exec_clients_[executor_idx].executorsched_finish_block(block_id);
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
  ElapsedTimer timer;
  trie.generate_top_level_hashes();
  double generate_top_level_hashes_duration = timer.elapsed();
  info("generate_top_level_hashes_duration {}",
       generate_top_level_hashes_duration);
  SHA256Hash root_hash = trie.get_root_hash();

  BlockHashPreimage preimage{.txs_merkle_trie_root_hash = root_hash,
                             .prev_block_hash = prev_block_hash};
  return sha256(preimage);
}

x23::Async<x23::Vec<bool>>
MempoolCoordinatorRpc::propose_batch(
    BlockID block_id, const x23::Vec<SignedTransaction> &txs,
    x23::XdrVec<NominatedTransaction> &transaction_nominations,
    size_t executor_idx)
{
  x23::Vec<bool> ret;
  ret.reserve(txs.size());

  ExecProposeBatchResult res =
      co_await exec_clients_[executor_idx].executorsched_propose_batch(block_id,
                                                                       txs);
  for (const ExecProposeResult &result : res) {
    switch (result.result()) {
    case ExecResult::ExecResultTxDeterministicError:
    case ExecResult::ExecResultOK:
      transaction_nominations.push_back(
          {.nondeterministic_proposal_data = result.proposal_data()});
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

  WorkerPool &worker_pool = WorkerPool::get_default_pool();
  size_t num_threads = 1; // worker_pool.worker_count();
  std::vector<std::future<void>> futures;
  futures.reserve(num_threads);

  size_t total_num_chunks = (txs.size() / chunk_size);
  if (txs.size() % chunk_size) {
    total_num_chunks++;
  }
  size_t chunks_per_thread = total_num_chunks / num_threads;
  x23::Vec<bool> ret(txs.size(), false);
  transaction_nominations.resize(txs.size());

  std::atomic<size_t> num_threads_done = 0;
  for (size_t thread_idx = 0; thread_idx < num_threads; thread_idx++) {
    futures.push_back(worker_pool.submit(
        [&num_threads_done, thread_idx, num_threads, total_num_chunks,
         chunks_per_thread, &ret, &transaction_nominations, chunk_size, &txs,
         block_id, this] {
          auto timer = std::make_unique<ElapsedTimer>();

          // Create the chunks.
          x23::Vec<x23::Vec<SignedTransaction>> chunks;
          size_t my_num_chunks = chunks_per_thread;
          if (total_num_chunks % num_threads != 0 &&
              thread_idx == num_threads - 1) {
            // Give the extra chunks to the last thread.
            my_num_chunks += total_num_chunks % num_threads;
          }
          chunks.reserve(my_num_chunks);
          for (size_t i = 0; i < my_num_chunks; i++) {
            size_t chunk_offset = thread_idx * chunks_per_thread + i;
            auto end = (chunk_offset < total_num_chunks - 1)
                           ? txs.begin() + ((chunk_offset + 1) * chunk_size)
                           : txs.end();
            chunks.emplace_back(txs.begin() + (chunk_offset * chunk_size), end);
            CHECK_LE(chunks.back().size(), chunk_size);
          }

          double create_chunks_duration = timer->elapsed();
          timer = std::make_unique<ElapsedTimer>();

          // Send the RPCs.
          x23::Vec<x23::Async<x23::Vec<bool>>> async_handles;
          x23::Vec<x23::XdrVec<NominatedTransaction>> nomination_chunks(
              chunks.size(), {});
          async_handles.reserve(chunks.size());
          for (size_t i = 0; i < chunks.size(); i++) {
            async_handles.push_back(
                propose_batch(block_id, chunks[i], nomination_chunks[i],
                              /*executor_idx=*/i % exec_clients_.size()));
          }

          double send_rpcs_duration = timer->elapsed();
          timer = std::make_unique<ElapsedTimer>();

          // Wait for the RPC responses.
          x23::Vec<x23::Vec<bool>> ret_chunks(chunks.size(), {});
          for (size_t i = 0; i < chunks.size(); i++) {
            // TODO(jhumphri): Is waiting synchronously on the handle ok?
            x23::Vec<bool> ret_chunk = async_handles[i]();
            ret_chunks[i] = ret_chunk;
          }

          double wait_for_response_duration = timer->elapsed();
          timer = std::make_unique<ElapsedTimer>();

          // Fold the results.
          size_t offset = thread_idx * chunks_per_thread * chunk_size;
          for (size_t i = 0; i < chunks.size(); i++) {
            CHECK_EQ(ret_chunks[i].size(), nomination_chunks[i].size());
            for (size_t j = 0; j < ret_chunks[i].size(); j++) {
              ret[offset] = ret_chunks[i][j];
              transaction_nominations[offset] = nomination_chunks[i][j];
              offset++;
            }
          }

          double fold_results_duration = timer->elapsed();
          if (thread_idx == 0) {
            info("create_chunks_duration {}, send_rpcs_duration {}, "
                 "wait_for_response_duration {}, fold_results_duration {}",
                 create_chunks_duration, send_rpcs_duration,
                 wait_for_response_duration, fold_results_duration);
          }
          num_threads_done++;
        }));
  }

  // TODO(jhumphri): This is a hack.
  while (num_threads_done < num_threads) {
    co_await default_coropoll().pool_.bg();
  }

  for (std::future<void> &future : futures) {
    future.get();
  }
  CHECK_EQ(ret.size(), txs.size());
  co_return ret;
}

x23::Async<ExecResult>
MempoolCoordinatorRpc::open_proposal_block(BlockID block_id,
                                           size_t executor_idx)
{
  return exec_clients_[executor_idx].executorsched_open_proposal_block(
      block_id);
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

  x23::Async<bool> create_snapshot_handler =
      create_snapshot(/*prev_block_id=*/db_tip.id, block_id,
                      BlockExecutionMode::kBlockExecutionModeProposal);
  x23::Vec<x23::Async<ExecResult>> open_proposal_block_handlers;
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    open_proposal_block_handlers.push_back(
        open_proposal_block(block_id, /*executor_idx=*/i));
  }

  // Scheduler ---
  auto timer2 = std::make_unique<ElapsedTimer>();

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

  x23::CoroGuard cg;
  auto timer3 = std::make_unique<ElapsedTimer>();
  co_await cg(scheduler_->global_lock_);
  double wait_for_lock_duration = timer3->elapsed();

  std::tie(nomination.transaction_hashes, txs) =
      scheduler_->schedule(kMaxBlockSize);
  size_t num_on_runqueue = scheduler_->size();
  cg.reset();

  double schedule_duration = timer2->elapsed();
  // Scheduler ---

  timer2 = std::make_unique<ElapsedTimer>();
  bool create_snapshot_handler_ret = co_await create_snapshot_handler;
  x23::Vec<ExecResult> open_proposal_block_handler_ret;
  bool open_proposal_block_success = true;
  for (x23::Async<ExecResult> &handler : open_proposal_block_handlers) {
    open_proposal_block_handler_ret.push_back(co_await handler);
    if (open_proposal_block_handler_ret.back() != ExecResult::ExecResultOK) {
      open_proposal_block_success = false;
    }
  }
  if (!create_snapshot_handler_ret || !open_proposal_block_success) {
    if (create_snapshot_handler_ret) {
      co_await abandon_snapshot(block_id);
    }
    for (size_t i = 0; i < open_proposal_block_handler_ret.size(); i++) {
      if (open_proposal_block_handler_ret[i] == ExecResult::ExecResultOK) {
        co_await finish_block(block_id, /*executor_idx=*/i);
      }
    }

    // TODO(jhumphri): Return the txs to the scheduler *and reset the hog
    // ages* and recover expired txs! Maybe we actually do not need to reset
    // the hog ages since failure is rare and one failure will not impact long
    // term fairness. Do we need to recover the expired txs either? A failure
    // is pretty bad and unexpected, and I do not think it is worth adding
    // complexity and overhead for recovery here for a rare event.
    scheduler_->decrement_current_block();
    co_await scheduler_->add_txs(txs);

    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  double wait_for_create_snapshot_and_open_block_handlers_duration =
      timer2->elapsed();
  double create_snapshot_and_open_block_and_schedule_duration =
      timer->elapsed();

  info("schedulercoord_nominate(), scheduler gave us {} txs, {} are runnable",
       txs.size(), num_on_runqueue);
  if (nomination.transaction_hashes.empty()) {
    // There are no vetted txs, so we cannot propose a block.
    for (size_t i = 0; i < exec_clients_.size(); i++) {
      co_await finish_block(block_id, /*executor_idx=*/i);
    }
    co_await abandon_snapshot(block_id);
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  timer = std::make_unique<ElapsedTimer>();

  // TODO(jhumphri): [propose_batch_in_chunks] should be parallelized.

  x23::Async<x23::Vec<bool>> propose_batch_in_chunks_handle =
      propose_batch_in_chunks(block_id, txs, nomination.transaction_nominations,
                              kChunkSize);

  x23::Vec<bool> statuses = co_await propose_batch_in_chunks_handle;
  double run_on_executor_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  x23::Vec<SignedTransaction> failed_txs;
  for (size_t i = 0; i < txs.size(); i++) {
    if (!statuses[i]) {
      failed_txs.push_back(txs[i]);
    }
  }
  if (!failed_txs.empty()) {
    co_await cg(scheduler_->global_lock_);
    co_await scheduler_->add_txs(failed_txs);
  }
  double mark_failed_txs_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  if (nomination.transaction_hashes.empty()) {
    // It is possible that all txs failed to execute, in which case the block
    // is empty. Thus, we fail the nomination.
    for (size_t i = 0; i < exec_clients_.size(); i++) {
      co_await finish_block(block_id, /*executor_idx=*/i);
    }
    co_await abandon_snapshot(block_id);
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  x23::Vec<x23::Async<void>> finish_block_handles;
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    finish_block_handles.push_back(finish_block(block_id, /*executor_idx=*/i));
  }
  x23::Async<void> close_snapshot_handle = close_snapshot(block_id);
  x23::Async<void> commit_snapshot_handle = commit_snapshot(block_id);
  x23::Async<void> mark_block_committed_handle = mark_block_committed(block_id);

  double create_handles_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  nomination.block_hash = get_block_hash(nomination.transaction_hashes,
                                         /*prev_block_hash=*/db_tip.hash);

  double get_block_hash_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  for (x23::Async<void> &handle : finish_block_handles) {
    co_await handle;
  }
  co_await close_snapshot_handle;
  co_await commit_snapshot_handle;
  co_await mark_block_committed_handle;

  double wait_for_handles_duration = timer->elapsed();

  info("wait_for_transfer_duration {}, "
       "create_snapshot_and_open_block_and_schedule_duration "
       "{}, wait_for_create_snapshot_and_open_block_handlers_duration {}, "
       "schedule_duration {}, "
       "run_on_executor_duration {}, "
       "mark_failed_txs_duration {}, create_handles_duration {}, "
       "get_block_hash_duration {}, wait_for_handles_duration {}, "
       "wait_for_lock_duration {}",
       wait_for_transfer_duration,
       create_snapshot_and_open_block_and_schedule_duration,
       wait_for_create_snapshot_and_open_block_handlers_duration,
       schedule_duration, run_on_executor_duration, mark_failed_txs_duration,
       create_handles_duration, get_block_hash_duration,
       wait_for_handles_duration, wait_for_lock_duration);

  co_return result;
}

x23::Async<bool>
MempoolCoordinatorRpc::validate_batch(BlockID block_id,
                                      const x23::Vec<ProposedTransaction> &txs,
                                      size_t executor_idx)
{
  ExecValidateBatchResult res =
      co_await exec_clients_[executor_idx].executorsched_validate_batch(
          block_id, txs);
  co_return !(std::ranges::any_of(res, [](const ExecResult &r) {
    return r != ExecResult::ExecResultOK;
  }));
}

x23::Async<ExecResult>
MempoolCoordinatorRpc::open_validation_block(BlockID block_id,
                                             size_t executor_idx)
{
  return exec_clients_[executor_idx].executorsched_open_validation_block(
      block_id);
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

  x23::Vec<x23::Async<ExecResult>> open_validation_block_handles;
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    open_validation_block_handles.push_back(
        open_validation_block(block_id, /*executor_idx=*/i));
  }
  bool open_validation_block_success = true;
  x23::Vec<ExecResult> open_validation_block_ret;
  for (x23::Async<ExecResult> &handle : open_validation_block_handles) {
    open_validation_block_ret.push_back(co_await handle);
    if (open_validation_block_ret.back() != ExecResult::ExecResultOK) {
      open_validation_block_success = false;
    }
  }
  if (!open_validation_block_success) {
    for (size_t i = 0; i < exec_clients_.size(); i++) {
      if (open_validation_block_ret[i] == ExecResult::ExecResultOK) {
        co_await finish_block(block_id, /*executor_idx=*/i);
      }
    }
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
            nomination.transaction_hashes, signed_txs)) {
      for (size_t i = 0; i < exec_clients_.size(); i++) {
        co_await finish_block(block_id, /*executor_idx=*/i);
      }
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

  // Careful--[validate_batch()] is calling co_await in its function body.
  bool validated = co_await validate_batch(block_id, txs, /*executor_idx=*/0);

  // Check [validated] and verify the block hash. It is safe to directly use
  // [nomination.transaction_hashes] here because we already checked above
  // that all of these hashes correspond to vetted txs.
  if (!validated || get_block_hash(nomination.transaction_hashes,
                                   /*prev_block_hash=*/db_tip.hash) !=
                        nomination.block_hash) {
    co_await scheduler_->add_txs(signed_txs);

    for (size_t i = 0; i < exec_clients_.size(); i++) {
      co_await finish_block(block_id, /*executor_idx=*/i);
    }
    co_await abandon_snapshot(block_id);
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

  for (size_t i = 0; i < exec_clients_.size(); i++) {
    co_await finish_block(block_id, /*executor_idx=*/i);
  }
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
