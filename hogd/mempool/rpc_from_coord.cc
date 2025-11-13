#include "hogd/mempool/rpc_from_coord.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <ctime>
#include <future>
#include <memory>
#include <span>
#include <vector>

#include "block.hh"
#include "coord_sched.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "exec_sched.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/merkle/allocator.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/container/merkle/node.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mempool/authentication.h"
#include "hogd/mempool/shared.h"
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
  co_await scheduler_->add_txs(authenticated);
  co_return co_await coord_client_->coordscheduler_notify_vetted(authenticated);
}

x23::Async<void>
MempoolCoordinatorRpc::schedulercoord_submit_vetted(
    ValidatorID validator_id, SignedTransactionBatch batch)
{
  auto timer = std::make_unique<ElapsedTimer>();

  co_await scheduler_->add_txs(batch);

  double add_txs_duration = timer->elapsed();
  info("mempool schedulercoord_submit_vetted add_txs_duration {}",
       add_txs_duration);

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

  MerkleTriePrefix trie(/*allocator_type=*/Allocator<Node>::Type::kBump);
  WorkerPool &worker_pool = WorkerPool::get_default_pool();
  // The optimal number of threads to use varies with the number of hashes we
  // are adding to the trie. For example, if we are adding 128 hashes to the
  // trie, it is probably slower to use 128 threads rather than 1-2 threads due
  // to lock contention.
  size_t num_threads = 108; // worker_pool.worker_count();
  std::vector<std::future<void>> futures;
  futures.reserve(num_threads);

  ElapsedTimer insert_timer;
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
  double insert_duration = insert_timer.elapsed();

  ElapsedTimer timer;
  trie.generate_top_level_hashes();
  double generate_top_level_hashes_duration = timer.elapsed();

  ElapsedTimer root_hash_timer;
  SHA256Hash root_hash = trie.get_root_hash();
  double root_hash_duration = root_hash_timer.elapsed();

  ElapsedTimer preimage_timer;
  BlockHashPreimage preimage{.txs_merkle_trie_root_hash = root_hash,
                             .prev_block_hash = prev_block_hash};
  auto ret = sha256(preimage);
  double preimage_duration = preimage_timer.elapsed();

  info("insert_duration {}, generate_top_level_hashes_duration {}, "
       "root_hash_duration {}, preimage_duration {}",
       insert_duration, generate_top_level_hashes_duration, root_hash_duration,
       preimage_duration);

  return ret;
}

x23::Detached
MempoolCoordinatorRpc::propose_batch(
    BlockID block_id, x23::span<const SignedTransaction> txs,
    x23::XdrVec<NominatedTransaction> &transaction_nominations,
    x23::span<bool> ret_chunk, size_t executor_idx, x23::CoroCB<void> cb)
{
  ExecProposeBatchResult res =
      co_await exec_clients_[executor_idx].executorsched_propose_batch(block_id,
                                                                       txs);
  for (size_t i = 0; i < res.size(); i++) {
    const ExecProposeResult &result = res[i];
    switch (result.result()) {
    case ExecResult::ExecResultTxDeterministicError:
    case ExecResult::ExecResultOK:
      transaction_nominations.push_back(
          {.nondeterministic_proposal_data = result.proposal_data()});
      ret_chunk[i] = true;
      break;
    default:
      ret_chunk[i] = false;
      break;
    }
  }
  cb();
}

x23::Detached
MempoolCoordinatorRpc::propose_small_chunks_from_large_chunk(
    BlockID block_id, x23::span<const SignedTransaction> txs,
    x23::Vec<x23::XdrVec<NominatedTransaction>> &nomination_chunks,
    x23::span<bool> ret, x23::CoroCB<void> cb)
{
  co_await default_coropoll().pool_.bg();

  size_t small_chunk_size = kChunkSize;
  size_t num_chunks = (txs.size() / small_chunk_size) +
                      ((txs.size() % small_chunk_size) ? 1 : 0);
  x23::Vec<std::unique_ptr<x23::Awaitable<void>>> awaitables;
  awaitables.reserve(num_chunks);
  nomination_chunks.resize(num_chunks, {});
  CHECK_EQ(ret.size(), txs.size());
  for (size_t i = 0; i < num_chunks; i++) {
    awaitables.push_back(std::make_unique<x23::Awaitable<void>>());

    size_t start_idx = i * small_chunk_size;
    size_t end_idx =
        (i < num_chunks - 1) ? start_idx + small_chunk_size : txs.size();
    x23::span<const SignedTransaction> chunk(txs.begin() + start_idx,
                                             txs.begin() + end_idx);
    x23::span<bool> ret_chunk(ret.begin() + start_idx, ret.begin() + end_idx);

    propose_batch(block_id, chunk, nomination_chunks[i], ret_chunk,
                  /*executor_idx=*/i % exec_clients_.size(),
                  x23::CoroCB<void>(*awaitables[i]));
  }

  for (size_t i = 0; i < num_chunks; i++) {
    co_await *awaitables[i];
  }
  cb();
}

x23::Detached
MempoolCoordinatorRpc::propose_batch_in_chunks(
    BlockID block_id, x23::span<SignedTransaction> txs,
    x23::XdrVec<NominatedTransaction> &transaction_nominations,
    x23::span<bool> ret, x23::CoroCB<void> cb)
{
  // Send the RPCs.
  constexpr size_t kNumBigChunks = 120;
  size_t large_chunk_size = txs.size() / kNumBigChunks;
  x23::Vec<std::unique_ptr<x23::Awaitable<void>>> awaitables;
  awaitables.reserve(kNumBigChunks);
  x23::Vec<x23::Vec<x23::XdrVec<NominatedTransaction>>> nomination_chunks(
      kNumBigChunks, {});
  CHECK_EQ(ret.size(), txs.size());
  for (size_t i = 0; i < kNumBigChunks; i++) {
    awaitables.push_back(std::make_unique<x23::Awaitable<void>>());

    size_t start_idx = i * large_chunk_size;
    size_t end_idx =
        (i < kNumBigChunks - 1) ? start_idx + large_chunk_size : txs.size();
    x23::span<const SignedTransaction> chunk(txs.begin() + start_idx,
                                             txs.begin() + end_idx);
    x23::span<bool> ret_chunk(ret.begin() + start_idx, ret.begin() + end_idx);

    propose_small_chunks_from_large_chunk(block_id, chunk, nomination_chunks[i],
                                          ret_chunk,
                                          x23::CoroCB<void>(*awaitables[i]));
  }

  // Wait for the RPC responses.
  transaction_nominations.reserve(txs.size());
  for (size_t i = 0; i < kNumBigChunks; i++) {
    co_await *awaitables[i];

    for (const x23::XdrVec<NominatedTransaction> &nominated_txs :
         nomination_chunks[i]) {
      transaction_nominations += nominated_txs;
    }
  }
  cb();
}

x23::Async<ExecResult>
MempoolCoordinatorRpc::open_proposal_block(BlockID block_id,
                                           size_t executor_idx)
{
  return exec_clients_[executor_idx].executorsched_open_proposal_block(
      block_id);
}

namespace {

std::unique_ptr<ElapsedTimer> nominate_return_timer =
    std::make_unique<ElapsedTimer>();

} // namespace

x23::Async<SchedulerNominationOutcome>
MempoolCoordinatorRpc::schedulercoord_nominate(DbTip db_tip)
{
  auto ret = schedulercoord_nominate_helper(db_tip);
  info("nominate return from function took {} s",
       nominate_return_timer->elapsed());
  return ret;
}

x23::Async<SchedulerNominationOutcome>
MempoolCoordinatorRpc::schedulercoord_nominate_helper(DbTip db_tip)
{
  static std::unique_ptr<ElapsedTimer> nominate_timer =
      std::make_unique<ElapsedTimer>();
  info("nominate: {} seconds between calls", nominate_timer->elapsed());
  // std::this_thread::sleep_for(std::chrono::milliseconds(20'000));

  // TODO(jhumphri): Replace with [CoroSem] when that is merged into main.
  auto timer = std::make_unique<ElapsedTimer>();
  while (!scheduler_->transfer_finished_) {
    co_await default_coropoll().pool_.bg();
  }
  scheduler_->transfer_finished_ = false;

  double wait_for_transfer_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  static x23::Vec<SignedTransaction> txs;
  static x23::Vec<SignedTransactionHash> tx_hashes;
  txs.clear();
  tx_hashes.clear();

  BlockID block_id = get_ns_since_epoch();

  x23::Async<bool> create_snapshot_handler =
      create_snapshot(/*prev_block_id=*/db_tip.id, block_id,
                      BlockExecutionMode::kBlockExecutionModeProposal);
  x23::Vec<x23::Async<ExecResult>> open_proposal_block_handlers;
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    open_proposal_block_handlers.push_back(
        open_proposal_block(block_id, /*executor_idx=*/i));
  }

  // This is the first call to [schedule()]. We do it up here rather than with
  // the remaining calls in the loop below because we want to overlap the first
  // call with opening the snapshot and the block.
  scheduler_->schedule(kMaxBlockSize / kNumSchedulerIterations, tx_hashes, txs);

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

    // Add the txs from the first call to [schedule()] above back to the
    // scheduler.
    // We do not reset the hog ages since failure is rare and one failure will
    // not impact long term fairness. We also do not recover expired txs. A
    // failure is pretty bad and unexpected, and I do not think it is worth
    // adding complexity and overhead for recovery here for a rare event.
    co_await scheduler_->add_txs(txs);
    scheduler_->decrement_current_block();

    co_return SchedulerNominationOutcome::SchedulerNominationError;
  }

  double wait_for_create_snapshot_and_open_block_handlers_duration =
      timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  // Scheduler ---

  static x23::Vec<SchedulerNominationChunk> sched_nominations(
      kNumSchedulerIterations, {});
  for (size_t i = 0; i < sched_nominations.size(); i++) {
    SchedulerNominationChunk &sched_nomination = sched_nominations[i];
    sched_nomination.chunk_idx = i;
    sched_nomination.total_chunks = kNumSchedulerIterations + 1;

    sched_nomination.chunk.block_id = block_id;
    Nomination &nomination = sched_nomination.chunk.nomination;
    nomination.block_height = db_tip.height + 1;
    nomination.prev_block_hash = db_tip.hash;
    nomination.transaction_hashes.clear();
    nomination.transaction_nominations.clear();
  }
  static SchedulerNominationChunk last_chunk;
  {
    last_chunk.chunk_idx = kNumSchedulerIterations;
    last_chunk.total_chunks = kNumSchedulerIterations + 1;
    last_chunk.chunk.block_id = block_id;

    Nomination &nomination = last_chunk.chunk.nomination;
    nomination.block_height = db_tip.height + 1;
    nomination.prev_block_hash = db_tip.hash;
    nomination.transaction_hashes.clear();
    nomination.transaction_nominations.clear();
  }

  // [nomination.block_hash], [nomination.transaction_hashes], and
  // [nomination.transaction_nominations] are filled in below.

  x23::CoroGuard cg;
  auto timer2 = std::make_unique<ElapsedTimer>();
  co_await cg(scheduler_->global_lock_);
  double wait_for_lock_duration = timer2->elapsed();

  x23::Vec<std::unique_ptr<x23::Awaitable<void>>>
      propose_batch_in_chunks_handles;
  propose_batch_in_chunks_handles.reserve(kNumSchedulerIterations);
  static x23::Vec<bool> statuses(kMaxBlockSize, false);
  static x23::Vec<x23::XdrVec<NominatedTransaction>>
      transaction_nomination_chunks;
  transaction_nomination_chunks.clear();
  transaction_nomination_chunks.resize(kNumSchedulerIterations, {});
  size_t num_scheduled = 0;
  for (size_t i = 0; i < kNumSchedulerIterations; i++) {
    if (i > 0) {
      // The first call to [schedule()] is done above since we want to overlap
      // it with opening the snapshot and the block.
      scheduler_->schedule(kMaxBlockSize / kNumSchedulerIterations, tx_hashes,
                           txs);
    }
    size_t num_scheduled_this_iteration = txs.size() - num_scheduled;
    if (!num_scheduled_this_iteration) {
      break;
    }

    // TODO(jhumphri): Might want to do this in another thread.
    propose_batch_in_chunks_handles.push_back(
        std::make_unique<x23::Awaitable<void>>());

    x23::span<bool> ret_span(statuses.begin() + num_scheduled,
                             statuses.begin() + tx_hashes.size());
    x23::span<SignedTransaction> txs_span(txs.begin() + num_scheduled,
                                          txs.end());
    num_scheduled = tx_hashes.size();
    propose_batch_in_chunks(block_id, txs_span,
                            transaction_nomination_chunks[i], ret_span,
                            x23::CoroCB<void>(
                                *propose_batch_in_chunks_handles.back()));
  }

  cg.reset();

  double schedule_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();
  if (tx_hashes.empty()) {
    // There are no vetted txs, so we cannot propose a block.
    for (size_t i = 0; i < exec_clients_.size(); i++) {
      co_await finish_block(block_id, /*executor_idx=*/i);
    }
    co_await abandon_snapshot(block_id);
    co_return SchedulerNominationOutcome::SchedulerNominationError;
  }
  // Scheduler ---

  timer2 = std::make_unique<ElapsedTimer>();

  for (size_t i = 0; i < propose_batch_in_chunks_handles.size(); i++) {
    co_await *propose_batch_in_chunks_handles[i];

    size_t total_nominations_so_far = i * kChunkSize;
    SchedulerNominationChunk &nomination =
        sched_nominations[total_nominations_so_far / kNumTxsPerNominationChunk];
    nomination.chunk.nomination.transaction_nominations +=
        transaction_nomination_chunks[i];
  }

  double just_wait_for_executor_duration = timer2->elapsed();
  double run_on_executor_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  static x23::Vec<SignedTransaction> failed_txs;
  failed_txs.clear();
  for (size_t i = 0; i < txs.size(); i++) {
    if (statuses[i]) {
      SchedulerNominationChunk &nomination =
          sched_nominations[i / kNumTxsPerNominationChunk];
      nomination.chunk.nomination.transaction_hashes.push_back(tx_hashes[i]);
    }
    else {
      failed_txs.push_back(txs[i]);
    }
  }
  if (!failed_txs.empty()) {
    co_await cg(scheduler_->global_lock_);
    co_await scheduler_->add_txs(failed_txs);
  }
  double mark_failed_txs_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  if (tx_hashes.empty()) {
    // It is possible that all txs failed to execute, in which case the block
    // is empty. Thus, we fail the nomination.
    for (size_t i = 0; i < exec_clients_.size(); i++) {
      co_await finish_block(block_id, /*executor_idx=*/i);
    }
    co_await abandon_snapshot(block_id);
    co_return SchedulerNominationOutcome::SchedulerNominationError;
  }

  x23::Vec<x23::Async<void>> finish_block_handles;
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    finish_block_handles.push_back(finish_block(block_id, /*executor_idx=*/i));
  }
  x23::Async<void> close_snapshot_handle = close_snapshot(block_id);
  x23::Async<void> commit_snapshot_handle = commit_snapshot(block_id);
  x23::Async<void> mark_block_committed_handle = mark_block_committed(block_id);

  x23::Vec<std::unique_ptr<x23::Awaitable<void>>> finish_nomination_handles;
  finish_nomination_handles.reserve(sched_nominations.size());
  for (size_t i = 0; i < sched_nominations.size(); i++) {
    sched_nominations[i].chunk_idx = i;
    finish_nomination_handles.push_back(
        std::make_unique<x23::Awaitable<void>>());

    [](std::unique_ptr<coord_sched_client_t> &coord_client,
       SchedulerNominationChunk &chunk,
       x23::CoroCB<void> cb) -> x23::Detached {
      co_await default_coropoll().pool_.bg();

      co_await coord_client->coordscheduler_finish_nomination(chunk);
      cb();
    }(coord_client_, sched_nominations[i],
                             x23::CoroCB<void>(
                                 *finish_nomination_handles.back()));
  }

  double create_handles_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  const SHA256Hash nomination_block_hash =
      get_block_hash(tx_hashes,
                     /*prev_block_hash=*/db_tip.hash);
  double get_block_hash_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  last_chunk.chunk_idx = sched_nominations.size();
  last_chunk.chunk.nomination.block_hash = nomination_block_hash;
  x23::Async<void> last_chunk_handle =
      coord_client_->coordscheduler_finish_nomination(last_chunk);

  for (x23::Async<void> &handle : finish_block_handles) {
    co_await handle;
  }
  co_await close_snapshot_handle;
  co_await commit_snapshot_handle;
  co_await mark_block_committed_handle;

  double wait_for_handles_duration = timer->elapsed();
  timer = std::make_unique<ElapsedTimer>();

  for (std::unique_ptr<x23::Awaitable<void>> &handle :
       finish_nomination_handles) {
    co_await *handle;
  }
  co_await last_chunk_handle;
  double wait_for_finish_nomination_handles = timer->elapsed();

  info("wait_for_transfer_duration {}, "
       "wait_for_create_snapshot_and_open_block_handlers_duration {}, "
       "schedule_duration {}, run_on_executor_duration {}, "
       "just_wait_for_executor_duration {}, "
       "mark_failed_txs_duration {}, create_handles_duration {}, "
       "get_block_hash_duration {}, wait_for_handles_duration {}, "
       "wait_for_finish_nomination_handles {}, wait_for_lock_duration {}",
       wait_for_transfer_duration,
       wait_for_create_snapshot_and_open_block_handlers_duration,
       schedule_duration, run_on_executor_duration,
       just_wait_for_executor_duration, mark_failed_txs_duration,
       create_handles_duration, get_block_hash_duration,
       wait_for_handles_duration, wait_for_finish_nomination_handles,
       wait_for_lock_duration);

  nominate_timer = std::make_unique<ElapsedTimer>();

  {
    struct timespec ts;
    // NOLINTBEGIN(misc-include-cleaner)
    clock_gettime(CLOCK_REALTIME, &ts);
    // NOLINTEND(misc-include-cleaner)
    int64_t ms = (ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000);
    info("will send nominate result: {}", ms);
  }

  nominate_return_timer = std::make_unique<ElapsedTimer>();
  co_return SchedulerNominationOutcome::SchedulerNominationOK;
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
