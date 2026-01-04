#include "hogd/mempool/rpc_from_coord.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <ctime>
#include <future>
#include <memory>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

#include "block.hh"
#include "coord_sched.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "event_rpc.hh"
#include "exec_result.hh"
#include "exec_sched.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/merkle/allocator.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/container/merkle/node.h"
#include "hogd/crypto/crypto.h"
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
  info("submit_unvetted, {} txs", batch.size());
  // TODO(jhumphri): Authenticate the txs. Is any other vetting needed?
  co_await scheduler_->add_txs(batch);
  co_await coord_client_->coordscheduler_notify_vetted(batch);
  co_return;
}

x23::Async<void>
MempoolCoordinatorRpc::schedulercoord_submit_vetted(
    ValidatorID validator_id, SignedTransactionBatch batch)
{
  info("submit_vetted, {} txs", batch.size());
  // We [co_await] rather than [return scheduler_->add_txs(batch)] directly
  // because [add_txs] takes the txs as a const reference, so we need [batch] to
  // stay in scope.
  co_await scheduler_->add_txs(batch);
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
MempoolCoordinatorRpc::eventsched_close_block(BlockID block_id)
{
  co_return co_await event_sched_client_->eventsched_close_block(block_id);
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
    x23::XdrVec<NominatedTransaction> &transaction_nominations,
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
      transaction_nominations.push_back(
          {.nondeterministic_proposal_data = result.proposal_data()});
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
    x23::Vec<x23::XdrVec<NominatedTransaction>> &nomination_chunks,
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
    x23::XdrVec<NominatedTransaction> &transaction_nominations,
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
  x23::Vec<x23::Vec<x23::XdrVec<NominatedTransaction>>> nomination_chunks(
      large_chunks.size(), {});
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
    for (const x23::XdrVec<NominatedTransaction> &nominated_txs :
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
  // Reset the nomination chunks so each scheduling iteration writes into a
  // clean slot.
  for (size_t i = 0; i < cache.sched_nominations.size(); i++) {
    SchedulerNominationChunk &sched_nomination = cache.sched_nominations[i];
    sched_nomination.chunk_idx = i;
    sched_nomination.total_chunks = kNumSchedulerIterations + 1;

    sched_nomination.chunk.block_id = cache.block_id;
    Nomination &nomination = sched_nomination.chunk.nomination;
    // TODO(jhumphri): Fill in the [proposer] field.
    nomination.block_height = cache.db_tip.height + 1;
    nomination.prev_block_hash = cache.db_tip.hash;
    nomination.transaction_hashes.clear();
    nomination.transaction_nominations.clear();
    // [nomination.block_hash] is filled in by [schedulercoord_nominate()].
    // [nomination.transaction_hashes] is written in [process_tx_outcomes] and
    // [nomination.transaction_nominations] is filled in below.
  }

  x23::Vec<x23::Async<void>> handles;
  handles.reserve(kNumSchedulerIterations);
  cache.transaction_nomination_chunks.clear();
  cache.transaction_nomination_chunks.resize(kNumSchedulerIterations, {});
  size_t num_scheduled = 0;
  for (size_t i = 0; i < kNumSchedulerIterations; i++) {
    if (i > 0) {
      // The first call to [schedule()] is done in [schedulercoord_nominate()]
      // since we want to overlap it with the block initialization RPCs.
      co_await scheduler_->schedule(kMaxBlockSize / kNumSchedulerIterations,
                                    cache.tx_hashes, cache.txs);
    }
    size_t num_scheduled_this_iteration = cache.txs.size() - num_scheduled;
    if (!num_scheduled_this_iteration) {
      break;
    }

    x23::span<bool> ret_span(cache.statuses.begin() + num_scheduled,
                             cache.statuses.begin() + cache.tx_hashes.size());
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

    size_t total_nominations_so_far = i * kChunkSize;
    SchedulerNominationChunk &nomination =
        cache.sched_nominations[total_nominations_so_far /
                                kNumTxsPerNominationChunk];
    nomination.chunk.nomination.transaction_nominations +=
        cache.transaction_nomination_chunks[i];
  }

  co_return;
}

void
MempoolCoordinatorRpc::process_tx_outcomes(PendingBlock &cache)
{
  cache.failed_txs.clear();

  for (size_t i = 0; i < cache.txs.size(); i++) {
    if (cache.statuses[i]) {
      SchedulerNominationChunk &nomination =
          cache.sched_nominations[i / kNumTxsPerNominationChunk];
      nomination.chunk.nomination.transaction_hashes.push_back(
          cache.tx_hashes[i]);
    }
    else {
      cache.failed_txs.push_back(cache.txs[i]);
    }
  }
}

x23::Async<void>
MempoolCoordinatorRpc::send_nomination_chunk(
    const SchedulerNominationChunk &chunk)
{
  co_return co_await coord_client_->coordscheduler_finish_nomination(chunk);
}

x23::Detached
MempoolCoordinatorRpc::finish_nominating_block(PendingBlock &cache)
{
  // Gracefully shut down executor work and send the nomination chunks
  // (including the final block hash) back to the coordinator.
  x23::Vec<x23::Async<void>> finish_block_handles;
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    finish_block_handles.push_back(
        finish_block(cache.block_id, /*executor_idx=*/i));
  }

  // Send the nomination chunks back to the coordinator.
  x23::Vec<x23::Async<void>> finish_nomination_handles;
  finish_nomination_handles.reserve(cache.sched_nominations.size());
  for (size_t i = 0; i < cache.sched_nominations.size(); i++) {
    cache.sched_nominations[i].chunk_idx = i;
    finish_nomination_handles.push_back(
        send_nomination_chunk(cache.sched_nominations[i]));
  }

  {
    // The last chunk is special. It does not include any tx nomination data.
    // Rather, it contains only the block hash. We create this special chunk and
    // place the block hash in it because we want to overlap the compute for
    // generating the block hash with the network I/O of sending the tx
    // nomination data to the coordinator. So if we instead placed the block
    // hash in all of the chunks, we could not overlap this compute.
    cache.last_chunk.chunk_idx = kNumSchedulerIterations;
    cache.last_chunk.total_chunks = kNumSchedulerIterations + 1;
    cache.last_chunk.chunk.block_id = cache.block_id;
    cache.last_chunk.chunk.nomination.block_hash =
        get_block_hash(cache.tx_hashes,
                       /*prev_block_hash=*/cache.db_tip.hash);

    Nomination &nomination = cache.last_chunk.chunk.nomination;
    nomination.block_height = cache.db_tip.height + 1;
    nomination.prev_block_hash = cache.db_tip.hash;
    CHECK(nomination.transaction_hashes.empty());
    CHECK(nomination.transaction_nominations.empty());
  }
  x23::Async<void> last_chunk_handle =
      coord_client_->coordscheduler_finish_nomination(cache.last_chunk);

  // [co_await] all of the handles.
  for (x23::Async<void> &handle : finish_block_handles) {
    co_await handle;
  }

  for (x23::Async<void> &handle : finish_nomination_handles) {
    co_await handle;
  }
  co_await last_chunk_handle;

  pending_detached_coroutines_.fetch_sub(1, std::memory_order_release);
  // Only one thread will be waiting on [pending_detached_coroutines_] in the
  // destructor.
  pending_detached_coroutines_.notify_one();
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
      eventsched_close_block(cache.block_id);
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
  info("NOMINATE!");
  x23::CoroGuard cg;
  co_await cg(nominated_block_cache_lock_);

  if (nominated_block_cache_.block_id) {
    // A block was nominated by this node and not yet committed or abandoned.
    // [schedulercoord_commit_block()] needs to be called before this node can
    // nominate another block.
    info("ret");
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  // Wait for hogdb to notify us of all HOG transfers that occurred in the
  // previous block. These transfer affect account balances, which in turn
  // affects scheduling priorities for accounts, so we need to wait for this
  // notification before scheduling the next block.
  co_await scheduler_->transfer_finished_.acquire();

  nominated_block_cache_.db_tip = db_tip;
  nominated_block_cache_.block_id = generate_block_id();
  // We place the block txs and tx hashes into these vectors. We mark these
  // vectors static and clear them on each call to [nominate()] so to avoid
  // memory allocation/free overhead.
  nominated_block_cache_.txs.clear();
  nominated_block_cache_.tx_hashes.clear();

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
    co_await scheduler_->schedule(kMaxBlockSize / kNumSchedulerIterations,
                                  nominated_block_cache_.tx_hashes,
                                  nominated_block_cache_.txs);
    info("schedule() gave us {} txs", nominated_block_cache_.tx_hashes.size());
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
    info("ret");
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  // Schedule the block and send the txs to the executors.
  co_await schedule(nominated_block_cache_);

  // Process the transaction outcomes.
  process_tx_outcomes(nominated_block_cache_);
  if (!nominated_block_cache_.failed_txs.empty()) {
    co_await scheduler_->add_txs(nominated_block_cache_.failed_txs);
  }
  // if (nominated_block_cache_.tx_hashes.empty() ||
  //     nominated_block_cache_.failed_txs.size() ==
  //         nominated_block_cache_.tx_hashes.size()) {
  //   // There are no txs for the block, so we cannot propose a block.
  //   co_await finish_all_executors(nominated_block_cache_.block_id);
  //   co_await abandon_block(nominated_block_cache_);
  //   info("ret");
  //   co_return SchedulerNominationResult(
  //       SchedulerNominationOutcome::SchedulerNominationError);
  // }

  pending_detached_coroutines_.fetch_add(1, std::memory_order_acquire);
  finish_nominating_block(nominated_block_cache_);

  SchedulerNominationResult result(
      SchedulerNominationOutcome::SchedulerNominationOK);
  result.block_id() = nominated_block_cache_.block_id;
  info("ret");
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

  // TODO(jhumphri): Need to ensure that the node that proposed the block is
  // following the expected scheduling policy.
  if (nomination.transaction_nominations.empty()) {
    // Empty nomination.
    co_await decrement_num_validations_in_progress();
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

  co_await scheduler_->transfer_finished_.acquire();

  // TODO(jhumphri): Remember that we need to pre-size the vectors in [cache]
  // correctly when we use the helper functions above to send txs to the
  // executors.
  PendingBlock cache{};
  cache.block_id = generate_block_id();
  cache.db_tip = db_tip;
  cache.tx_hashes = nomination.transaction_hashes;
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
    for (const SignedTransactionHash &tx_hash : nomination.transaction_hashes) {
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
    CHECK_EQ(cache.txs.size(), nomination.transaction_nominations.size());
    for (size_t i = 0; i < cache.txs.size(); i++) {
      txs.push_back({.signed_tx = cache.txs[i],
                     .nondeterministic_proposal_data =
                         nomination.transaction_nominations[i]
                             .nondeterministic_proposal_data});
    }
  }
  CHECK(!txs.empty());

  // Now execute the txs on the executor.

  // Careful--[validate_batch()] is calling co_await in its function body.
  bool validated =
      co_await validate_batch(cache.block_id, txs, /*executor_idx=*/0);

  // Check [validated] and verify the block hash. It is safe to directly use
  // [nomination.transaction_hashes] here because we already checked above
  // that all of these hashes correspond to vetted txs.
  if (!validated || get_block_hash(nomination.transaction_hashes,
                                   /*prev_block_hash=*/cache.db_tip.hash) !=
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
  info("schedulercoord_commit_block!!!");
  x23::CoroGuard validate_cg;
  co_await validate_cg(validated_block_cache_lock_);
  if (num_validations_in_progress_) {
    info("schedulercoord_commit_block ret");
    co_return SchedulerCommitOutcome::SchedulerCommitRace;
  }
  x23::CoroGuard nominate_cg;
  co_await nominate_cg(nominated_block_cache_lock_);

  if ((!nominated_block_cache_.block_id ||
       nominated_block_cache_.block_id != block_id) &&
      !validated_block_cache_.contains(block_id)) {
    // A block with ID [block_id] was neither nominated nor validated, so it
    // cannot be committed.
    info("schedulercoord_commit_block ret");
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
  info("schedulercoord_commit_block ret");
  co_return SchedulerCommitOutcome::SchedulerCommitOK;
}

} // namespace hogd
