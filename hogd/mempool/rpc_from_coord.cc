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
#include "event_rpc.hh"
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

namespace {

struct ChunkRange {
  size_t start;
  size_t end; // [start, end)
};

std::vector<ChunkRange>
make_chunks(size_t total_size, size_t chunk_count)
{
  std::vector<ChunkRange> chunks;
  if (!total_size || !chunk_count) {
    return chunks;
  }
  chunk_count = std::min(chunk_count, total_size);

  const size_t base_size = total_size / chunk_count;
  const size_t remainder = total_size % chunk_count;

  size_t offset = 0;
  chunks.reserve(chunk_count);
  for (size_t i = 0; i < chunk_count; ++i) {
    const size_t chunk_size = base_size + (i < remainder ? 1 : 0);
    const size_t start = offset;
    const size_t end = start + chunk_size;
    chunks.push_back({start, end});
    offset = end;
  }
  CHECK_EQ(offset, total_size);

  return chunks;
}

std::vector<ChunkRange>
make_chunks_for_max_size(size_t total_size, size_t max_chunk_size)
{
  if (!max_chunk_size) {
    return {};
  }
  const size_t chunk_count = (total_size + max_chunk_size - 1) / max_chunk_size;
  return make_chunks(total_size, chunk_count);
}

} // namespace

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
  // We [co_await] rather than [return] because [add_txs] takes the txs as a
  // const reference, so we need [batch] to stay in scope.
  co_await scheduler_->add_txs(batch);
}

x23::Async<DbSchedResult>
MempoolCoordinatorRpc::create_snapshot(BlockID prev_block_id, BlockID block_id,
                                       BlockExecutionMode mode)
{
  return db_sched_client_->dbsched_create_snapshot(prev_block_id, block_id,
                                                   mode);
}

x23::Async<RPCError>
MempoolCoordinatorRpc::eventsched_open_block(BlockID block_id)
{
  return event_sched_client_->eventsched_open_block(block_id);
}

x23::Async<DbSchedResult>
MempoolCoordinatorRpc::abandon_snapshot(BlockID block_id)
{
  return db_sched_client_->dbsched_abandon_snapshot(block_id);
}

x23::Async<RPCError>
MempoolCoordinatorRpc::eventsched_abandon_block(BlockID block_id)
{
  return event_sched_client_->eventsched_abandon_block(block_id);
}

x23::Async<DbBlockResult>
MempoolCoordinatorRpc::close_snapshot(BlockID block_id)
{
  return db_sched_client_->dbsched_close_snapshot(block_id);
  // TODO(geoff): forward the root hash of the event block back to the block
  // header
  // TODO(geoff): consider whether this should be called from commit instead
}

x23::Async<EventBlockResult>
MempoolCoordinatorRpc::eventsched_close_block(BlockID block_id)
{
  return event_sched_client_->eventsched_close_block(block_id);
}

x23::Async<DbBlockResult>
MempoolCoordinatorRpc::commit_snapshot(BlockID block_id)
{
  return db_sched_client_->dbsched_commit_snapshot(block_id);
}

x23::Async<void>
MempoolCoordinatorRpc::finish_block(BlockID block_id, size_t executor_idx)
{
  return exec_clients_[executor_idx].executorsched_finish_block(block_id);
}

SHA256Hash
MempoolCoordinatorRpc::get_block_hash(
    const x23::Vec<SignedTransactionHash> &hashes,
    const BlockHash &prev_block_hash)
{
  CHECK(!hashes.empty());

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

// This function spawns a coroutine and sends [txs] to the executors. The
// coroutine splits [txs] into smaller chunks so that (1) the smaller chunks can
// be multiplexed across multiple TCP connections and (2) the executor can begin
// executing txs without having to wait for *all* txs to arrive first.
x23::Detached
MempoolCoordinatorRpc::propose_small_chunks_from_large_chunk(
    BlockID block_id, x23::span<const SignedTransaction> txs,
    x23::Vec<x23::XdrVec<NominatedTransaction>> &nomination_chunks,
    x23::span<bool> ret, x23::CoroCB<void> cb)
{
  co_await default_coropoll().pool_.bg();

  const std::vector<ChunkRange> chunks =
      make_chunks_for_max_size(txs.size(), kChunkSize);

  x23::Vec<std::unique_ptr<x23::Awaitable<void>>> awaitables;
  awaitables.reserve(chunks.size());

  nomination_chunks.resize(chunks.size(), {});
  CHECK_EQ(ret.size(), txs.size());

  // Each iteration in this loop creates a small chunk and sends it to the
  // executor via [propose_batch()].
  for (size_t i = 0; i < chunks.size(); i++) {
    awaitables.push_back(std::make_unique<x23::Awaitable<void>>());

    const ChunkRange &chunk = chunks[i];
    x23::span<const SignedTransaction> tx_chunk(txs.begin() + chunk.start,
                                                txs.begin() + chunk.end);
    x23::span<bool> ret_chunk(ret.begin() + chunk.start,
                              ret.begin() + chunk.end);

    // We multiplex the small chunks across multiple TCP connections and
    // multiple executors by setting the [executor_idx] to [i %
    // exec_clients_.size()].
    propose_batch(block_id, tx_chunk, nomination_chunks[i], ret_chunk,
                  /*executor_idx=*/i % exec_clients_.size(),
                  x23::CoroCB<void>(*awaitables[i]));
  }

  // Wait for the executor to finish executing the txs.
  for (size_t i = 0; i < chunks.size(); i++) {
    co_await *awaitables[i];
  }

  // Notify the calling coroutine that we are finished.
  cb();
}

x23::Detached
MempoolCoordinatorRpc::propose_batch_in_chunks(
    BlockID block_id, x23::span<SignedTransaction> txs,
    x23::XdrVec<NominatedTransaction> &transaction_nominations,
    x23::span<bool> ret, x23::CoroCB<void> cb)
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

  // The coroutines notify us when they are done via these [awaitables]. There
  // is one awaitable per coroutine.
  x23::Vec<std::unique_ptr<x23::Awaitable<void>>> awaitables;
  awaitables.reserve(large_chunks.size());

  // The nomination data is placed into this vector, with one entry reserved for
  // each coroutine.
  x23::Vec<x23::Vec<x23::XdrVec<NominatedTransaction>>> nomination_chunks(
      large_chunks.size(), {});
  // The tx outcomes are written to [ret].
  CHECK_EQ(ret.size(), txs.size());
  for (size_t i = 0; i < large_chunks.size(); i++) {
    awaitables.push_back(std::make_unique<x23::Awaitable<void>>());

    const ChunkRange &chunk = large_chunks[i];
    x23::span<const SignedTransaction> tx_chunk(txs.begin() + chunk.start,
                                                txs.begin() + chunk.end);
    // The outcome of each tx ([true] for success and [false] for failure) is
    // written into this span.
    x23::span<bool> status_chunk(ret.begin() + chunk.start,
                                 ret.begin() + chunk.end);

    propose_small_chunks_from_large_chunk(block_id, tx_chunk,
                                          nomination_chunks[i], status_chunk,
                                          x23::CoroCB<void>(*awaitables[i]));
  }

  // Wait for the RPC responses.
  transaction_nominations.reserve(txs.size());
  for (size_t i = 0; i < large_chunks.size(); i++) {
    co_await *awaitables[i];

    // Collect the nomination data into the [transaction_nominations] vector.
    for (const x23::XdrVec<NominatedTransaction> &nominated_txs :
         nomination_chunks[i]) {
      transaction_nominations += nominated_txs;
    }
  }

  // Notify the calling coroutine that we are finished.
  cb();
}

x23::Async<ExecResult>
MempoolCoordinatorRpc::open_proposal_block(BlockID block_id,
                                           size_t executor_idx)
{
  return exec_clients_[executor_idx].executorsched_open_proposal_block(
      block_id);
}

x23::Async<bool>
MempoolCoordinatorRpc::initialize_block(const NominationData &cache)
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
        open_proposal_block(cache.block_id, /*executor_idx=*/i));
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
    if (create_snapshot_handler_ret == DbSchedResult::DbSchedResultOK) {
      CHECK_EQ(co_await abandon_snapshot(cache.block_id),
               DbSchedResult::DbSchedResultOK);
    }
    CHECK_EQ(co_await eventsched_abandon_block(cache.block_id),
             RPCError::kRPCNone);
    for (size_t i = 0; i < open_proposal_block_handler_ret.size(); i++) {
      if (open_proposal_block_handler_ret[i] == ExecResult::ExecResultOK) {
        co_await finish_block(cache.block_id, /*executor_idx=*/i);
      }
    }

    co_return false;
  }
  co_return true;
}

x23::Async<void>
MempoolCoordinatorRpc::schedule(NominationData &cache)
{
  x23::CoroGuard cg;
  co_await cg(scheduler_->global_lock_);
  for (size_t i = 0; i < cache.sched_nominations.size(); i++) {
    SchedulerNominationChunk &sched_nomination = cache.sched_nominations[i];
    sched_nomination.chunk_idx = i;
    sched_nomination.total_chunks = kNumSchedulerIterations + 1;

    sched_nomination.chunk.block_id = cache.block_id;
    Nomination &nomination = sched_nomination.chunk.nomination;
    nomination.block_height = cache.db_tip.height + 1;
    nomination.prev_block_hash = cache.db_tip.hash;
    nomination.transaction_hashes.clear();
    nomination.transaction_nominations.clear();
    // [nomination.block_hash] is filled in by [schedulercoord_nominate()].
    // [nomination.transaction_hashes] and [nomination.transaction_nominations]
    // are filled in below.
  }

  x23::Vec<std::unique_ptr<x23::Awaitable<void>>>
      propose_batch_in_chunks_handles;
  propose_batch_in_chunks_handles.reserve(kNumSchedulerIterations);
  cache.transaction_nomination_chunks.clear();
  cache.transaction_nomination_chunks.resize(kNumSchedulerIterations, {});
  size_t num_scheduled = 0;
  for (size_t i = 0; i < kNumSchedulerIterations; i++) {
    if (i > 0) {
      // The first call to [schedule()] is done above since we want to overlap
      // it with opening the snapshot and the block.
      scheduler_->schedule(kMaxBlockSize / kNumSchedulerIterations,
                           cache.tx_hashes, cache.txs);
    }
    size_t num_scheduled_this_iteration = cache.txs.size() - num_scheduled;
    if (!num_scheduled_this_iteration) {
      break;
    }

    // TODO(jhumphri): Might want to do this in another thread.
    propose_batch_in_chunks_handles.push_back(
        std::make_unique<x23::Awaitable<void>>());

    x23::span<bool> ret_span(cache.statuses.begin() + num_scheduled,
                             cache.statuses.begin() + cache.tx_hashes.size());
    x23::span<SignedTransaction> txs_span(cache.txs.begin() + num_scheduled,
                                          cache.txs.end());
    num_scheduled = cache.tx_hashes.size();
    propose_batch_in_chunks(cache.block_id, txs_span,
                            cache.transaction_nomination_chunks[i], ret_span,
                            x23::CoroCB<void>(
                                *propose_batch_in_chunks_handles.back()));
  }

  for (size_t i = 0; i < propose_batch_in_chunks_handles.size(); i++) {
    co_await *propose_batch_in_chunks_handles[i];

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
MempoolCoordinatorRpc::process_tx_outcomes(NominationData &cache)
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
MempoolCoordinatorRpc::finish_nominating_block(NominationData &cache)
{
  x23::Vec<x23::Async<void>> finish_block_handles;
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    finish_block_handles.push_back(
        finish_block(cache.block_id, /*executor_idx=*/i));
  }
  x23::Async<DbBlockResult> close_snapshot_handle =
      close_snapshot(cache.block_id);
  x23::Async<EventBlockResult> close_block_handle =
      eventsched_close_block(cache.block_id);
  x23::Async<DbBlockResult> commit_snapshot_handle =
      commit_snapshot(cache.block_id);
  x23::Async<void> mark_block_committed_handle =
      mark_block_committed(cache.block_id);

  x23::Vec<std::unique_ptr<x23::Awaitable<void>>> finish_nomination_handles;
  finish_nomination_handles.reserve(cache.sched_nominations.size());
  for (size_t i = 0; i < cache.sched_nominations.size(); i++) {
    cache.sched_nominations[i].chunk_idx = i;
    finish_nomination_handles.push_back(
        std::make_unique<x23::Awaitable<void>>());

    [](std::unique_ptr<coord_sched_client_t> &coord_client,
       SchedulerNominationChunk &chunk,
       x23::CoroCB<void> cb) -> x23::Detached {
      co_await default_coropoll().pool_.bg();

      co_await coord_client->coordscheduler_finish_nomination(chunk);
      cb();
    }(coord_client_, cache.sched_nominations[i],
                             x23::CoroCB<void>(
                                 *finish_nomination_handles.back()));
  }

  {
    cache.last_chunk.chunk_idx = kNumSchedulerIterations;
    cache.last_chunk.total_chunks = kNumSchedulerIterations + 1;
    cache.last_chunk.chunk.block_id = cache.block_id;
    cache.last_chunk.chunk.nomination.block_hash =
        get_block_hash(cache.tx_hashes,
                       /*prev_block_hash=*/cache.db_tip.hash);

    Nomination &nomination = cache.last_chunk.chunk.nomination;
    nomination.block_height = cache.db_tip.height + 1;
    nomination.prev_block_hash = cache.db_tip.hash;
    nomination.transaction_hashes.clear();
    nomination.transaction_nominations.clear();
  }
  x23::Async<void> last_chunk_handle =
      coord_client_->coordscheduler_finish_nomination(cache.last_chunk);

  for (x23::Async<void> &handle : finish_block_handles) {
    co_await handle;
  }
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
  co_await mark_block_committed_handle;

  for (std::unique_ptr<x23::Awaitable<void>> &handle :
       finish_nomination_handles) {
    co_await *handle;
  }
  co_await last_chunk_handle;
}

x23::Async<SchedulerNominationOutcome>
MempoolCoordinatorRpc::schedulercoord_nominate(DbTip db_tip)
{
  // Wait for hogdb to notify us of all HOG transfers that occurred in the
  // previous block. These transfer affect account balances, which in turn
  // affects scheduling priorities for accounts, so we need to wait for this
  // notification before scheduling the next block.
  co_await scheduler_->transfer_finished_.acquire();

  static NominationData cache;
  cache.block_id = get_ns_since_epoch();
  // We place the block txs and tx hashes into these vectors. We mark these
  // vectors static and clear them on each call to [nominate()] so to avoid
  // memory allocation/free overhead.
  cache.txs.clear();
  cache.tx_hashes.clear();

  // Block initialization work. We sent these RPCs concurrently and [co_await]
  // them below. This block initialization work takes ~10ms, so we schedule the
  // first batch of txs below while these RPCs are outstanding and before we
  // [co_await] them.
  x23::Async<bool> initialize_block_handle = initialize_block(cache);

  {
    // This is the first call to [schedule()]. We do it up here rather than with
    // the other calls in the loop below because we want to overlap the first
    // call with the block initialization work.
    x23::CoroGuard cg;
    co_await cg(scheduler_->global_lock_);
    scheduler_->schedule(kMaxBlockSize / kNumSchedulerIterations,
                         cache.tx_hashes, cache.txs);
  }

  if (!co_await initialize_block_handle) {
    // Add the txs from the first call to [schedule()] above back to the
    // scheduler.
    // We do not reset the hog ages since failure is rare and one failure will
    // not impact long term fairness. We also do not recover expired txs. A
    // failure is pretty bad and unexpected, and I do not think it is worth
    // adding complexity and overhead in the common case to for full recovery
    // from a rare event.
    co_await scheduler_->add_txs(cache.txs);
    scheduler_->decrement_current_block();

    co_return SchedulerNominationOutcome::SchedulerNominationError;
  }

  // Schedule the block and send the txs to the executors.
  co_await schedule(cache);

  // Process the transaction outcomes.
  process_tx_outcomes(cache);
  if (!cache.failed_txs.empty()) {
    co_await scheduler_->add_txs(cache.failed_txs);
  }
  if (cache.tx_hashes.empty() ||
      cache.failed_txs.size() == cache.tx_hashes.size()) {
    // There are no txs for the block, so we cannot propose a block.
    for (size_t i = 0; i < exec_clients_.size(); i++) {
      co_await finish_block(cache.block_id, /*executor_idx=*/i);
    }
    CHECK_EQ(co_await abandon_snapshot(cache.block_id),
             DbSchedResult::DbSchedResultOK);
    CHECK_EQ(co_await eventsched_abandon_block(cache.block_id),
             RPCError::kRPCNone);

    // [scheduler_->scheduler()] increments the block number, so we need to
    // decrement it since we did not nominate a block.
    scheduler_->decrement_current_block();
    co_return SchedulerNominationOutcome::SchedulerNominationError;
  }

  co_await finish_nominating_block(cache);
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

  co_await scheduler_->transfer_finished_.acquire();

  BlockID block_id = get_ns_since_epoch();
  if (co_await create_snapshot(
          /*prev_block_id=*/db_tip.id, block_id,
          BlockExecutionMode::kBlockExecutionModeValidation) !=
      DbSchedResult::DbSchedResultOK) {
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationDbFailed);
  }
  CHECK_EQ(co_await eventsched_open_block(block_id), RPCError::kRPCNone);

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
    CHECK_EQ(co_await abandon_snapshot(block_id),
             DbSchedResult::DbSchedResultOK);
    co_await eventsched_abandon_block(block_id);
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
      CHECK_EQ(co_await abandon_snapshot(block_id),
               DbSchedResult::DbSchedResultOK);
      co_await eventsched_abandon_block(block_id);
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
    CHECK_EQ(co_await abandon_snapshot(block_id),
             DbSchedResult::DbSchedResultOK);
    co_await eventsched_abandon_block(block_id);
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

  for (size_t i = 0; i < exec_clients_.size(); i++) {
    co_await finish_block(block_id, /*executor_idx=*/i);
  }
  {
    DbBlockResult result = co_await close_snapshot(block_id);
    CHECK_EQ(result.result(), DbSchedResult::DbSchedResultOK);
  }
  {
    EventBlockResult result = co_await eventsched_close_block(block_id);
    CHECK_EQ(result.error(), RPCError::kRPCNone);
  }
  {
    DbBlockResult result = co_await commit_snapshot(block_id);
    CHECK_EQ(result.result(), DbSchedResult::DbSchedResultOK);
  }
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
