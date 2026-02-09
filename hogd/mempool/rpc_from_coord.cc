#include "hogd/mempool/rpc_from_coord.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <future>
#include <memory>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

#include "auth_rpc.hh"
#include "block.hh"
#include "coord_sched.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "event_rpc.hh"
#include "exec_result.hh"
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
#include "hogd/mempool/shared.h"
#include "nomination.hh"
#include "parallel.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/vec.h"

namespace hogd {

namespace {

template<typename T>
void
remove_indices(x23::XdrVec<T> &remove_from, const x23::Vec<size_t> indices)
{
  size_t write_idx = 0;
  size_t num_erased = 0;
  for (size_t read_idx = 0; read_idx < remove_from.size(); read_idx++) {
    if (num_erased < indices.size() && read_idx == indices[num_erased]) {
      // We are erasing this element, so skip it.
      ++num_erased;
    }
    else {
      remove_from[write_idx++] = std::move(remove_from[read_idx]);
    }
  }
  remove_from.resize(write_idx);
}

} // namespace

x23::Async<void>
MempoolCoordinatorRpc::schedulercoord_submit_unvetted(
    ParallelSignedTransactionBatch batch)
{
  SignedTransactionBatch txs;
  CHECK(co_await parallel_batch_to_t(batch, txs));

  x23::CoroGuard cg;
  co_await cg(commit_lock_);

  ElapsedTimer timer;
  info("submit_unvetted, {} txs", txs.size());
  AuthErrorCodeBatch auth_ret = co_await auth_->verify_transactions(
      /*height=*/scheduler_->current_block(), txs, /*max_parallelism=*/128);
  info("auth took {}", timer.elapsed());

  x23::Vec<SignedTransactionHash> to_remove;
  x23::Vec<size_t> to_remove_indices;
  for (size_t i = 0; i < auth_ret.size(); i++) {
    const AuthErrorCode &code = auth_ret[i];
    if (code != AuthErrorCode::kAuthSuccess) {
      to_remove.push_back(sha256(txs[i].tx));
      to_remove_indices.push_back(i);
    }
  }
  info("{} txs were invalid", to_remove.size());
  if (!to_remove.empty()) {
    remove_indices(/*remove_from=*/txs, /*indices=*/to_remove_indices);
  }
  // Careful: it is tempting to overlap the overhead of
  // [auth_->verify_transactions()] with that of [scheduler_->add_txs()] (and
  // then after remove any unverified txs from the scheduler). But this would
  // make the txs visible to nominate() and validate() before the txs have been
  // verified.
  auto add_txs_handle = scheduler_->add_txs(txs);

  // TODO(jhumphri): We could avoid serializing [txs] and reuse [batch] if we do
  // not remove any txs above. I am not sure this is the common case though.
  ParallelBatch response;
  co_await t_to_parallel_batch(txs, response);
  co_await coord_client_->coordscheduler_notify_vetted(response);

  co_await add_txs_handle;

  info("about to return from submit_unvetted, scheduler.size() = {}, duration "
       "= {}",
       scheduler_->size(), timer.elapsed());
  co_return;
}

x23::Async<void>
MempoolCoordinatorRpc::schedulercoord_submit_vetted(
    ValidatorID validator_id, ParallelSignedTransactionBatch batch)
{
  SignedTransactionBatch txs;
  CHECK(co_await parallel_batch_to_t(batch, txs));
  info("submit_vetted, {} txs", txs.size());
  co_await scheduler_->add_txs(txs);
  co_return;
}

x23::Async<DbSchedResult>
MempoolCoordinatorRpc::create_snapshot(BlockID prev_block_id, BlockID block_id,
                                       BlockExecutionMode mode)
{
  co_return co_await db_sched_client_->dbsched_create_snapshot(prev_block_id,
                                                               block_id, mode);
}

x23::Async<RPCError>
MempoolCoordinatorRpc::eventsched_open_block(BlockID block_id)
{
  co_return co_await event_sched_client_->eventsched_open_block(block_id);
}

x23::Async<DbSchedResult>
MempoolCoordinatorRpc::abandon_snapshot(BlockID block_id)
{
  co_return co_await db_sched_client_->dbsched_abandon_snapshot(block_id);
}

x23::Async<RPCError>
MempoolCoordinatorRpc::eventsched_abandon_block(BlockID block_id)
{
  co_return co_await event_sched_client_->eventsched_abandon_block(block_id);
}

x23::Async<DbBlockResult>
MempoolCoordinatorRpc::close_snapshot(BlockID block_id)
{
  co_return co_await db_sched_client_->dbsched_close_snapshot(block_id);
  // TODO(geoff): forward the root hash of the event block back to the block
  // header
  // TODO(geoff): consider whether this should be called from commit instead
}

x23::Async<EventBlockResult>
MempoolCoordinatorRpc::eventsched_close_block(BlockID block_id,
                                              BlockHeight block_height)
{
  co_return co_await event_sched_client_->eventsched_close_block(block_id,
                                                                 block_height);
}

x23::Async<DbBlockResult>
MempoolCoordinatorRpc::commit_snapshot(BlockID block_id)
{
  co_return co_await db_sched_client_->dbsched_commit_snapshot(block_id);
}

x23::Async<void>
MempoolCoordinatorRpc::finish_block(BlockID block_id, size_t executor_idx)
{
  auto r =
      co_await exec_clients_[executor_idx].executorsched_finish_block(block_id);
  CHECK_EQ(r, ExecResult::ExecResultOK);
  co_return;
}

x23::Async<void>
MempoolCoordinatorRpc::finish_all_executors(BlockID block_id)
{
  // Best-effort cleanup helper used when we need to shut down/finish a block
  // across all executor clients in a uniform way.
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    co_await finish_block(block_id, /*executor_idx=*/i);
  }
}

SHA256Hash
MempoolCoordinatorRpc::get_block_hash(
    const x23::XdrVec<SignedTransactionHash> &hashes,
    const BlockHash &prev_block_hash)
{
  MerkleTriePrefix trie(/*allocator_type=*/Allocator<Node>::Type::kBump);
  WorkerPool &worker_pool = WorkerPool::get_default_pool();
  // The optimal number of threads to use varies with the number of hashes we
  // are adding to the trie. For example, if we are adding 50 hashes to the
  // trie, it is probably slower to use 128 threads rather than 1-2 threads due
  // to lock contention.
  constexpr size_t kNumThreads = 108;
  std::vector<std::future<void>> futures;
  const std::vector<ChunkRange> chunks =
      make_chunks(hashes.size(), kNumThreads);
  futures.reserve(chunks.size());

  for (const ChunkRange &chunk : chunks) {
    futures.push_back(worker_pool.submit([chunk, &trie, &hashes] {
      for (size_t j = chunk.start; j < chunk.end; j++) {
        // We add each tx hash to the Merkle trie as a key with an empty value.
        // So in essence, we are using the Merkle trie as a set.
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

x23::Async<void>
MempoolCoordinatorRpc::propose_batch(
    BlockID block_id, x23::span<const SignedTransaction> txs,
    x23::XdrVec<NondeterministicProposalData> &transaction_nominations,
    x23::span<bool> ret_chunk, size_t executor_idx)
{
  ExecProposeBatchResult res =
      co_await exec_clients_[executor_idx].executorsched_propose_batch(block_id,
                                                                       txs);
  // Record nomination metadata for txs that succeeded on the executor and mark
  // the per-tx status in [ret_chunk] so upstream callers can rebuild summaries.
  for (size_t i = 0; i < res.size(); i++) {
    const ExecProposeResult &result = res[i];

    switch (result.result()) {
    case ExecResult::ExecResultTxDeterministicError:
    case ExecResult::ExecResultOK:
    case ExecResult::ExecResultTxRootFrameReverted:
      transaction_nominations.push_back(result.proposal_data());
      ret_chunk[i] = true;
      break;
    default:
      ret_chunk[i] = false;
      break;
    }
  }
}

// This function spawns a coroutine and sends [txs] to the executors. The
// coroutine splits [txs] into smaller chunks so that (1) the smaller chunks can
// be multiplexed across multiple TCP connections and (2) the executor can begin
// executing txs without having to wait for *all* txs to arrive first.
x23::Async<void>
MempoolCoordinatorRpc::multiplex_large_chunk_across_executors(
    BlockID block_id, x23::span<const SignedTransaction> txs,
    x23::Vec<x23::XdrVec<NondeterministicProposalData>> &nomination_chunks,
    x23::span<bool> ret)
{
  co_await default_coropoll().pool_.bg();

  const std::vector<ChunkRange> chunks =
      make_chunks_for_max_size(txs.size(), kChunkSize);

  x23::Vec<x23::Async<void>> handles;
  handles.reserve(chunks.size());

  nomination_chunks.resize(chunks.size(), {});
  CHECK_EQ(ret.size(), txs.size());

  // Each iteration in this loop creates a small chunk and sends it to the
  // executor via [propose_batch()].
  for (size_t i = 0; i < chunks.size(); i++) {
    const ChunkRange &chunk = chunks[i];
    x23::span<const SignedTransaction> tx_chunk(txs.begin() + chunk.start,
                                                txs.begin() + chunk.end);
    x23::span<bool> ret_chunk(ret.begin() + chunk.start,
                              ret.begin() + chunk.end);

    // We multiplex the small chunks across multiple TCP connections and
    // multiple executors by setting the [executor_idx] to [i %
    // exec_clients_.size()].
    handles.push_back(propose_batch(block_id, tx_chunk, nomination_chunks[i],
                                    ret_chunk,
                                    /*executor_idx=*/i % exec_clients_.size()));
  }

  // Wait for the executor to finish executing the txs.
  for (size_t i = 0; i < chunks.size(); i++) {
    co_await handles[i];
  }
}

x23::Async<void>
MempoolCoordinatorRpc::propose_batch_in_chunks(
    BlockID block_id, x23::span<SignedTransaction> txs,
    x23::XdrVec<NondeterministicProposalData> &transaction_nominations,
    x23::span<bool> ret)
{
  // We split the [txs] into [kNumBigChunks] chunks and then send each chunk
  // from a different coroutine. Each coroutine then further splits the chunk
  // into even smaller chunks. This latter split is useful so that (1) we can
  // send the smaller chunks across multiple TCP streams and (2) so that the
  // executor can begin executing txs without having to wait for *all* of the
  // txs to arrive first.
  //
  // Each coroutine will likely be scheduled on its own thread, so I choose to
  // create 120 chunks (one per coroutine) since the machine has 128 logical
  // cores and I want to leave some cores leftover for other work.
  constexpr size_t kNumBigChunks = 120;
  const std::vector<ChunkRange> large_chunks =
      make_chunks(txs.size(), kNumBigChunks);

  x23::Vec<x23::Async<void>> handles;
  handles.reserve(large_chunks.size());

  // The nomination data is placed into this vector, with one entry reserved for
  // each coroutine.
  x23::Vec<x23::Vec<x23::XdrVec<NondeterministicProposalData>>>
      nomination_chunks(large_chunks.size(), {});
  // The tx outcomes are written to [ret].
  CHECK_EQ(ret.size(), txs.size());
  for (size_t i = 0; i < large_chunks.size(); i++) {
    const ChunkRange &chunk = large_chunks[i];
    x23::span<const SignedTransaction> tx_chunk(txs.begin() + chunk.start,
                                                txs.begin() + chunk.end);
    // The outcome of each tx ([true] for success and [false] for failure) is
    // written into this span.
    x23::span<bool> status_chunk(ret.begin() + chunk.start,
                                 ret.begin() + chunk.end);

    handles.push_back(multiplex_large_chunk_across_executors(
        block_id, tx_chunk, nomination_chunks[i], status_chunk));
  }

  // Wait for the RPC responses.
  transaction_nominations.reserve(txs.size());
  for (size_t i = 0; i < large_chunks.size(); i++) {
    co_await handles[i];

    // Collect the nomination data into the [transaction_nominations] vector.
    for (const x23::XdrVec<NondeterministicProposalData> &nominated_txs :
         nomination_chunks[i]) {
      transaction_nominations += nominated_txs;
    }
  }
}

x23::Async<ExecResult>
MempoolCoordinatorRpc::open_proposal_block(BlockID block_id,
                                           size_t executor_idx)
{
  return exec_clients_[executor_idx].executorsched_open_proposal_block(
      block_id);
}

x23::Async<bool>
MempoolCoordinatorRpc::initialize_block(const PendingBlock &cache)
{
  // Create snapshot.
  x23::Async<DbSchedResult> create_snapshot_handler =
      create_snapshot(/*prev_block_id=*/cache.db_tip.id, cache.block_id,
                      BlockExecutionMode::kBlockExecutionModeProposal);

  // Open block (on event server).
  x23::Async<RPCError> open_block_handler =
      eventsched_open_block(cache.block_id);

  // Open proposal blocks.
  x23::Vec<x23::Async<ExecResult>> open_proposal_block_handlers;
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    open_proposal_block_handlers.push_back(
        open_proposal_block(cache.block_id,
                            /*executor_idx=*/i));
  }

  // [co_await] the block initialization RPCs.
  DbSchedResult create_snapshot_handler_ret = co_await create_snapshot_handler;
  CHECK_EQ(co_await open_block_handler, RPCError::kRPCNone);

  x23::Vec<ExecResult> open_proposal_block_handler_ret;
  bool open_proposal_block_success = true;
  for (x23::Async<ExecResult> &handler : open_proposal_block_handlers) {
    open_proposal_block_handler_ret.push_back(co_await handler);
    if (open_proposal_block_handler_ret.back() != ExecResult::ExecResultOK) {
      open_proposal_block_success = false;
    }
  }

  if (create_snapshot_handler_ret != DbSchedResult::DbSchedResultOK ||
      !open_proposal_block_success) {
    // One of the RPCs failed, so we need to undo any of the successful RPC
    // calls.
    if (create_snapshot_handler_ret == DbSchedResult::DbSchedResultOK) {
      CHECK_EQ(co_await abandon_snapshot(cache.block_id),
               DbSchedResult::DbSchedResultOK);
    }
    CHECK_EQ(co_await eventsched_abandon_block(cache.block_id),
             RPCError::kRPCNone);
    for (size_t i = 0; i < open_proposal_block_handler_ret.size(); i++) {
      if (open_proposal_block_handler_ret[i] == ExecResult::ExecResultOK) {
        co_await finish_block(cache.block_id,
                              /*executor_idx=*/i);
      }
    }

    co_return false;
  }
  co_return true;
}

x23::Async<void>
MempoolCoordinatorRpc::schedule(PendingBlock &cache)
{
  x23::CoroGuard cg;
  co_await cg(scheduler_->global_lock_);

  x23::Vec<x23::Async<void>> handles;
  handles.reserve(kNumSchedulerIterations);
  cache.transaction_nomination_chunks.clear();
  cache.transaction_nomination_chunks.resize(kNumSchedulerIterations, {});
  size_t num_scheduled = 0;
  for (size_t i = 0; i < kNumSchedulerIterations; i++) {
    if (i > 0) {
      // The first call to [schedule()] is done in [schedulercoord_nominate()]
      // since we want to overlap it with the block initialization RPCs.
      ElapsedTimer timer;
      scheduler_->schedule(kMaxBlockSize / kNumSchedulerIterations,
                           cache.tx_hashes, cache.txs);
      info("schedule() took {}, scheduler size is {}", timer.elapsed(),
           scheduler_->size());
    }
    size_t num_scheduled_this_iteration = cache.txs.size() - num_scheduled;
    if (!num_scheduled_this_iteration) {
      break;
    }

    x23::span<bool> ret_span(cache.statuses.begin() + num_scheduled,
                             cache.statuses.begin() + cache.tx_hashes.size());
    // [schedule()] appends transactions to the end of [txs]. Normally,
    // [txs_span] could therefore become invalidated because the backing store
    // of [txs] could be resized on an append. However, we reserve the backing
    // store of [txs] up front with
    // [nominated_block_cache_.txs.reserve(kMaxBlockSize)] in
    // [schedulercoord_nominate()], so we do not need to worry about the span
    // becoming invalidated.
    x23::span<SignedTransaction> txs_span(cache.txs.begin() + num_scheduled,
                                          cache.txs.end());
    num_scheduled = cache.tx_hashes.size();
    handles.push_back(
        propose_batch_in_chunks(cache.block_id, txs_span,
                                cache.transaction_nomination_chunks[i],
                                ret_span));
  }

  for (size_t i = 0; i < handles.size(); i++) {
    co_await handles[i];
    cache.result_tx_nominations += cache.transaction_nomination_chunks[i];
  }

  co_return;
}

void
MempoolCoordinatorRpc::process_tx_outcomes(PendingBlock &cache)
{
  cache.failed_txs.clear();

  for (size_t i = 0; i < cache.txs.size(); i++) {
    if (cache.statuses[i]) {
      cache.result_tx_hashes.push_back(cache.tx_hashes[i]);
    }
    else {
      cache.failed_txs.push_back(cache.txs[i]);
    }
  }
}

x23::Async<void>
MempoolCoordinatorRpc::finish_nominating_block(const DbTip &db_tip,
                                               PendingBlock &cache)
{
  // Gracefully shut down executor work and send the nomination chunks
  // (including the final block hash) back to the coordinator.
  x23::Vec<x23::Async<void>> finish_block_handles;
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    finish_block_handles.push_back(
        finish_block(cache.block_id, /*executor_idx=*/i));
  }

  {
    Nomination &nomination =
        cache.nomination_result.nominated_block().nomination;
    nomination.block_height = db_tip.height + 1;
    nomination.block_hash = get_block_hash(cache.tx_hashes,
                                           /*prev_block_hash=*/db_tip.hash);
    nomination.prev_block_hash = db_tip.hash;
    nomination.transactions_hash = {};
    nomination.transactions_nondeterministic_data = {};
    co_await t_to_parallel_batch(cache.result_tx_hashes,
                                 nomination.transactions_hash);
    co_await t_to_parallel_batch(cache.result_tx_nominations,
                                 nomination.transactions_nondeterministic_data);
  }

  for (x23::Async<void> &handle : finish_block_handles) {
    co_await handle;
  }
  co_return;
}

x23::Async<void>
MempoolCoordinatorRpc::abandon_block(const PendingBlock &cache)
{
  CHECK_EQ(co_await abandon_snapshot(cache.block_id),
           DbSchedResult::DbSchedResultOK);
  CHECK_EQ(co_await eventsched_abandon_block(cache.block_id),
           RPCError::kRPCNone);
}

x23::Async<void>
MempoolCoordinatorRpc::commit_block(const PendingBlock &cache)
{
  x23::Async<DbBlockResult> close_snapshot_handle =
      close_snapshot(cache.block_id);
  x23::Async<EventBlockResult> close_block_handle =
      eventsched_close_block(cache.block_id, scheduler_->current_block());
  x23::Async<DbBlockResult> commit_snapshot_handle =
      commit_snapshot(cache.block_id);

  {
    DbBlockResult result = co_await close_snapshot_handle;
    CHECK_EQ(result.result(), DbSchedResult::DbSchedResultOK);
  }
  {
    EventBlockResult result = co_await close_block_handle;
    CHECK_EQ(result.error(), RPCError::kRPCNone);
  }
  {
    DbBlockResult result = co_await commit_snapshot_handle;
    CHECK_EQ(result.result(), DbSchedResult::DbSchedResultOK);
  }
}

x23::Async<SchedulerNominationResult>
MempoolCoordinatorRpc::schedulercoord_nominate(DbTip db_tip)
{
  ElapsedTimer timer;
  std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration delta = end - last_;
  // NOLINTNEXTLINE(modernize-use-auto):
  std::chrono::milliseconds delta_raw =
      std::chrono::duration_cast<std::chrono::milliseconds>(delta);
  int64_t delta_count = delta_raw.count();
  info("NOMINATE, delta = {}", delta_count);

  x23::CoroGuard cg;
  co_await cg(nominated_block_cache_lock_);

  if (db_tip != last_committed_block_) {
    last_ = std::chrono::steady_clock::now();
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }
  if (nominated_block_cache_.block_id) {
    // A block was nominated by this node and not yet committed or abandoned.
    // [schedulercoord_commit_block()] needs to be called before this node can
    // nominate another block.
    last_ = std::chrono::steady_clock::now();
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  // Wait for hogdb to notify us of all HOG transfers that occurred in the
  // previous block. These transfer affect account balances, which in turn
  // affects scheduling priorities for accounts, so we need to wait for this
  // notification before scheduling the next block.
  co_await scheduler_->transfer_finished_.acquire();
  co_await wait_for_auth_data(db_tip.height);

  nominated_block_cache_.block_id = generate_block_id();
  // We place the block txs and tx hashes into these vectors. We mark these
  // vectors static and clear them on each call to [nominate()] so to avoid
  // memory allocation/free overhead.
  nominated_block_cache_.txs.clear();
  nominated_block_cache_.tx_hashes.clear();
  nominated_block_cache_.result_tx_hashes.clear();
  nominated_block_cache_.result_tx_nominations.clear();
  nominated_block_cache_.txs.reserve(kMaxBlockSize);
  nominated_block_cache_.tx_hashes.reserve(kMaxBlockSize);
  nominated_block_cache_.result_tx_hashes.reserve(kMaxBlockSize);
  nominated_block_cache_.result_tx_nominations.reserve(kMaxBlockSize);
  // We set the outcome here because doing so resets
  // [nominated_block_cache_.nomination_result.nominated_block()]. We fill in
  // [nominated_block()] throughout this function, so we want our writes to it
  // preserved.
  nominated_block_cache_.nomination_result.outcome(
      SchedulerNominationOutcome::SchedulerNominationOK);

  // Block initialization work. We sent these RPCs concurrently and [co_await]
  // them below. This block initialization work takes ~10ms, so we schedule the
  // first batch of txs below while these RPCs are outstanding and before we
  // [co_await] them.
  x23::Async<bool> initialize_block_handle =
      initialize_block(nominated_block_cache_);

  {
    // This is the first call to [schedule()]. We do it up here rather than with
    // the other calls in the loop below because we want to overlap the first
    // call with the block initialization work.
    x23::CoroGuard cg;
    co_await cg(scheduler_->global_lock_);
    scheduler_->schedule(kMaxBlockSize / kNumSchedulerIterations,
                         nominated_block_cache_.tx_hashes,
                         nominated_block_cache_.txs);
    info("schedule() gave us {} txs", nominated_block_cache_.tx_hashes.size());
    if (nominated_block_cache_.tx_hashes.size() > 0) {
      info("one of the hashes is: {}",
           HexArray(nominated_block_cache_.tx_hashes[0]).str());
    }
  }
  if (!co_await initialize_block_handle) {
    // Add the txs from the first call to [schedule()] above back to the
    // scheduler.
    // We do not reset the hog ages since failure is rare and one failure will
    // not impact long term fairness. We also do not recover expired txs. A
    // failure is pretty bad and unexpected, and I do not think it is worth
    // adding complexity and overhead in the common case to for full recovery
    // from a rare event.
    co_await scheduler_->add_txs(nominated_block_cache_.txs);
    last_ = std::chrono::steady_clock::now();
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  // Schedule the block and send the txs to the executors.
  co_await schedule(nominated_block_cache_);
  info("schedule() gave us {} txs, scheduler size is {}",
       nominated_block_cache_.tx_hashes.size(), scheduler_->size());

  // Process the transaction outcomes.
  process_tx_outcomes(nominated_block_cache_);
  if (!nominated_block_cache_.failed_txs.empty()) {
    co_await scheduler_->add_txs(nominated_block_cache_.failed_txs);
  }

  co_await finish_nominating_block(db_tip, nominated_block_cache_);

  // We set [nominated_block_cache_.nomination_result.outcome()] to
  // [SchedulerNominationOutcome::SchedulerNominationOK] above. See the comments
  // there.
  nominated_block_cache_.nomination_result.nominated_block().block_id =
      nominated_block_cache_.block_id;
  nominated_block_cache_.db_tip = {
      .height = nominated_block_cache_.nomination_result.nominated_block()
                    .nomination.block_height,
      .hash = nominated_block_cache_.nomination_result.nominated_block()
                  .nomination.block_hash,
      .id =
          nominated_block_cache_.nomination_result.nominated_block().block_id};
  info("nominate() took {}", timer.elapsed());
  last_ = std::chrono::steady_clock::now();
  co_return nominated_block_cache_.nomination_result;
}

x23::Async<bool>
MempoolCoordinatorRpc::validate_batch(BlockID block_id,
                                      const x23::Vec<ProposedTransaction> &txs,
                                      size_t executor_idx)
{
  ExecValidateBatchResult res =
      co_await exec_clients_[executor_idx].executorsched_validate_batch(
          block_id, txs);
  // Validation succeeds only if every entry in the executor response is OK.
  co_return !(std::ranges::any_of(res, [](const ExecResult &r) {
    return r != ExecResult::ExecResultOK;
  }));
}

x23::Async<ExecResult>
MempoolCoordinatorRpc::open_validation_block(BlockID block_id,
                                             size_t executor_idx)
{
  co_return co_await exec_clients_[executor_idx]
      .executorsched_open_validation_block(block_id);
}

x23::Async<SchedulerValidationResult>
MempoolCoordinatorRpc::schedulercoord_validate(DbTip db_tip,
                                               Nomination nomination)
{
  co_await increment_num_validations_in_progress();

  if (db_tip.height + 1 != nomination.block_height) {
    co_await decrement_num_validations_in_progress();
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }
  if (db_tip.hash != nomination.prev_block_hash) {
    co_await decrement_num_validations_in_progress();
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }
  if (nomination.prev_block_hash != last_committed_block_.hash ||
      nomination.block_height != last_committed_block_.height + 1) {
    co_await decrement_num_validations_in_progress();
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

  // TODO(jhumphri): Need to ensure that the node that proposed the block is
  // following the expected scheduling policy.
  x23::XdrVec<NondeterministicProposalData> tx_nominations;
  CHECK(co_await parallel_batch_to_t(
      nomination.transactions_nondeterministic_data, tx_nominations));
  if (tx_nominations.empty()) {
    // Empty nomination.
    co_await decrement_num_validations_in_progress();
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

  // TODO(jhumphri): Remember that we need to pre-size the vectors in [cache]
  // correctly when we use the helper functions above to send txs to the
  // executors.
  PendingBlock cache{};
  cache.block_id = generate_block_id();
  cache.db_tip = {.height = nomination.block_height,
                  .hash = nomination.block_hash,
                  .id = cache.block_id};
  CHECK(co_await parallel_batch_to_t(nomination.transactions_hash,
                                     cache.tx_hashes));

  co_await scheduler_->transfer_finished_.acquire();
  co_await wait_for_auth_data(nomination.block_height - 1);

  if (co_await create_snapshot(
          /*prev_block_id=*/cache.db_tip.id, cache.block_id,
          BlockExecutionMode::kBlockExecutionModeValidation) !=
      DbSchedResult::DbSchedResultOK) {
    co_await decrement_num_validations_in_progress();
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationDbFailed);
  }
  CHECK_EQ(co_await eventsched_open_block(cache.block_id), RPCError::kRPCNone);

  x23::Vec<x23::Async<ExecResult>> open_validation_block_handles;
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    open_validation_block_handles.push_back(
        open_validation_block(cache.block_id, /*executor_idx=*/i));
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
        co_await finish_block(cache.block_id, /*executor_idx=*/i);
      }
    }
    co_await abandon_block(cache);
    co_await decrement_num_validations_in_progress();
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationExecutorFailed);
  }

  // [nomination] includes the the tx hashes and the nondeterministic proposal
  // data. We need to use these tx hashes to look up the vetted txs in
  // [vetted_txs_]. Assuming we find all of the txs, we then combine them with
  // the nondeterminstic proposal data to form a [ProposedTransaction], which
  // is what the executors expect to receive.
  x23::Vec<ProposedTransaction> txs;
  {
    for (const SignedTransactionHash &tx_hash : cache.tx_hashes) {
      SignedTransaction *tx = scheduler_->find_tx(tx_hash);
      if (!tx) {
        co_await finish_all_executors(cache.block_id);
        co_await abandon_block(cache);
        co_await decrement_num_validations_in_progress();
        co_return SchedulerValidationResult(
            SchedulerValidationOutcome::SchedulerValidationMissingTxs);
      }
      cache.txs.push_back(*tx);
    }
    x23::XdrVec<NondeterministicProposalData> tx_nominations;
    CHECK(co_await parallel_batch_to_t(
        nomination.transactions_nondeterministic_data, tx_nominations));
    CHECK_EQ(cache.txs.size(), tx_nominations.size());
    for (size_t i = 0; i < cache.txs.size(); i++) {
      txs.push_back({.signed_tx = cache.txs[i],
                     .nondeterministic_proposal_data = tx_nominations[i]});
    }
  }
  CHECK(!txs.empty());

  // Now execute the txs on the executor.

  // Careful--[validate_batch()] is calling co_await in its function body.
  bool validated =
      co_await validate_batch(cache.block_id, txs, /*executor_idx=*/0);

  // Check [validated] and verify the block hash. It is safe to directly use
  // [cache.tx_hashes] here because we already checked above that all of these
  // hashes correspond to vetted txs.
  if (!validated ||
      get_block_hash(cache.tx_hashes,
                     /*prev_block_hash=*/nomination.prev_block_hash) !=
          nomination.block_hash) {
    co_await scheduler_->add_txs(cache.txs);

    co_await finish_all_executors(cache.block_id);
    co_await abandon_block(cache);
    co_await decrement_num_validations_in_progress();
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

  co_await finish_all_executors(cache.block_id);

  cache.db_tip = DbTip{.height = nomination.block_height,
                       .hash = nomination.block_hash,
                       .id = cache.block_id};
  SchedulerValidationResult result(
      SchedulerValidationOutcome::SchedulerValidationOK);
  result.db_tip() = cache.db_tip;

  {
    x23::CoroGuard cg;
    co_await cg(validated_block_cache_lock_);
    CHECK(!validated_block_cache_.contains(cache.block_id));
    validated_block_cache_[cache.block_id] = std::move(cache);

    // We already hold [validated_block_cache_lock_], so I decrement
    // [num_validations_in_progress_] directly rather than call
    // [decrement_num_validations_in_progress()].
    CHECK_GT(num_validations_in_progress_, 0UL);
    num_validations_in_progress_--;
  }
  co_return result;
}

x23::Async<SchedulerCommitOutcome>
MempoolCoordinatorRpc::schedulercoord_commit_block(BlockID block_id)
{
  x23::CoroGuard commit_cg;
  co_await commit_cg(commit_lock_);

  std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration delta = end - last_;
  // NOLINTNEXTLINE(modernize-use-auto):
  std::chrono::milliseconds delta_raw =
      std::chrono::duration_cast<std::chrono::milliseconds>(delta);
  int64_t delta_count = delta_raw.count();
  info("schedulercoord_commit_block, delta = {}", delta_count);
  x23::CoroGuard validate_cg;
  co_await validate_cg(validated_block_cache_lock_);
  if (num_validations_in_progress_) {
    last_ = std::chrono::steady_clock::now();
    co_return SchedulerCommitOutcome::SchedulerCommitRace;
  }
  x23::CoroGuard nominate_cg;
  co_await nominate_cg(nominated_block_cache_lock_);

  if ((!nominated_block_cache_.block_id ||
       nominated_block_cache_.block_id != block_id) &&
      !validated_block_cache_.contains(block_id)) {
    // A block with ID [block_id] was neither nominated nor validated, so it
    // cannot be committed.
    last_ = std::chrono::steady_clock::now();
    co_return SchedulerCommitOutcome::SchedulerCommitBlockNotFound;
  }

  x23::Vec<x23::Async<void>> handles;
  handles.reserve(validated_block_cache_.size());
  if (nominated_block_cache_.block_id &&
      nominated_block_cache_.block_id == block_id) {
    // We are committing the block that we nominated.
    handles.push_back(commit_block(nominated_block_cache_));
    last_committed_block_ = nominated_block_cache_.db_tip;
  }
  else {
    // We are committing a block that we validated.
    handles.push_back(commit_block(validated_block_cache_[block_id]));
    if (nominated_block_cache_.block_id) {
      // We nominated a block, so we need to abandon the block.
      handles.push_back(abandon_block(nominated_block_cache_));
    }
    last_committed_block_ = validated_block_cache_[block_id].db_tip;
  }
  for (const auto &[_, cache] : validated_block_cache_) {
    if (cache.block_id != block_id) {
      handles.push_back(abandon_block(cache));
    }
  }

  // We need to do two things:
  // 1. If the nominated block is not the committed block, we need to return
  // the nominated block's txs to the scheduler.
  //
  // 2. Next, we need to remove the committed block's txs from the scheduler.
  // Note that some of these txs may overlap with the txs in the nominated
  // block, so we can optimize this by only returning to the scheduler the
  // txs in the nominated block that are not in the committed block. Then we
  // can remove all of the committed block's txs from the scheduler.
  static std::unordered_set<SignedTransactionHash> return_to_runqueue;
  return_to_runqueue.clear();
  const x23::XdrVec<SignedTransactionHash> *remove_from_scheduler = nullptr;
  if (nominated_block_cache_.block_id &&
      nominated_block_cache_.block_id == block_id) {
    // The committed block is the block we nominated.
    remove_from_scheduler = &nominated_block_cache_.tx_hashes;
  }
  else {
    // The committed block is a block we validated.
    if (nominated_block_cache_.block_id &&
        nominated_block_cache_.block_id != block_id) {
      // Return the nominated block's txs to the runqueue so they can be
      // scheduled again.
      return_to_runqueue.insert(nominated_block_cache_.tx_hashes.begin(),
                                nominated_block_cache_.tx_hashes.end());
    }
    for (const auto &[_, cache] : validated_block_cache_) {
      if (cache.block_id == block_id) {
        remove_from_scheduler = &cache.tx_hashes;
      }
    }
    CHECK_CONST_PTR(remove_from_scheduler);
    // Any of the txs in the committed (validated) block that were also in the
    // nominated block should not be returned to the runqueue.
    for (const SignedTransactionHash &hash : *remove_from_scheduler) {
      if (return_to_runqueue.empty()) {
        break;
      }
      return_to_runqueue.erase(hash);
    }
  }

  CHECK_CONST_PTR(remove_from_scheduler);
  scheduler_->reenqueue_txs(return_to_runqueue.cbegin(),
                            return_to_runqueue.cend());
  co_await scheduler_->remove_from_scheduler(
      x23::span<const SignedTransactionHash>(*remove_from_scheduler),
      /*validated_block=*/(nominated_block_cache_.block_id == 0UL) ||
          nominated_block_cache_.block_id != block_id);

  for (x23::Async<void> &handle : handles) {
    co_await handle;
  }

  // Be careful to not do [nominated_block_cache_ = {}]. We pre-allocate
  // elements in the vectors in [nominated_block_cache_], so if we clear those
  // vectors, we will experience segfaults because we will later attempt to
  // read/write indices that do not exist.
  nominated_block_cache_.block_id = 0;
  validated_block_cache_.clear();

  scheduler_->increment_current_block();
  last_ = std::chrono::steady_clock::now();
  co_return SchedulerCommitOutcome::SchedulerCommitOK;
}

// TODO(jhumphri): After a commit, I think I need to check that
// [auth_data_changed] was called before any subsequent calls to nominate() or
// validate(). Also be sure to make sure this works with the genesis block.
// I do not need to check this in add_txs--this is all kept in sync by the
// [auth_data_changed] callback.
//
// Also [transfer_finished_] should not be a CoroSem, it should instead be set
// to the last block height seen in the callback.
x23::Async<void>
MempoolCoordinatorRpc::auth_data_changed(BlockHeight height,
                                         const KeyChangeBatch &key_changes)
{
  info("auth_data_changed, height = {}, key_changes.size() = {}", height,
       key_changes.size());

  CHECK_EQ(height, auth_data_height_.load() + 1);

  x23::CoroGuard cg;
  co_await cg(commit_lock_);

  static SignedTransactionHashBatch tx_hashes;
  static SignedTransactionBatch txs;
  tx_hashes.clear();
  txs.clear();
  for (const KeyChange &change : key_changes) {
    // Get all txs for [change.address] and re-verify them. Remove any that fail
    // verification.
    static SignedTransactionHashBatch change_tx_hashes;
    static SignedTransactionBatch change_txs;
    change_tx_hashes.clear();
    change_txs.clear();
    scheduler_->get_txs_for_account(change.address, change_tx_hashes,
                                    change_txs);
    tx_hashes += change_tx_hashes;
    txs += change_txs;
  }
  AuthErrorCodeBatch auth_ret = co_await auth_->verify_transactions(
      /*height=*/scheduler_->current_block(), txs);
  x23::Vec<SignedTransactionHash> to_remove;
  for (size_t i = 0; i < auth_ret.size(); i++) {
    const AuthErrorCode &code = auth_ret[i];
    if (code != AuthErrorCode::kAuthSuccess) {
      to_remove.push_back(tx_hashes[i]);
    }
  }
  // The txs are on the runqueue and need to be removed, so we set
  // [validated_block] to true.
  co_await scheduler_->remove_from_scheduler(to_remove,
                                             /*validated_block=*/true);
  auth_data_height_.store(height);
  auth_data_height_.broadcast(&default_threadpool());
}

} // namespace hogd
