#include "hogd/mempool/rpc_from_coord.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
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
#include "hogd/base/os.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/base/xdr_util.h"
#include "hogd/container/merkle/allocator.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/container/merkle/node.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mempool/scheduler.h"
#include "nomination.hh"
#include "parallel.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/corosync.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

template<typename T>
void
remove_indices(x23::XdrVec<T> &remove_from, const x23::Vec<size_t> &indices)
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

auto
validation_outcome_str(SchedulerValidationOutcome outcome) -> const char *
{
  switch (outcome) {
  case SchedulerValidationOutcome::SchedulerValidationOK:
    return "ok";
  case SchedulerValidationOutcome::SchedulerValidationInvalid:
    return "invalid";
  case SchedulerValidationOutcome::SchedulerValidationTooLateToValidate:
    return "too_late";
  case SchedulerValidationOutcome::SchedulerValidationMissingTxs:
    return "missing_txs";
  case SchedulerValidationOutcome::SchedulerValidationUnknownParentBlock:
    return "unknown_parent";
  case SchedulerValidationOutcome::SchedulerValidationDbFailed:
    return "db_failed";
  case SchedulerValidationOutcome::SchedulerValidationExecutorFailed:
    return "executor_failed";
  case SchedulerValidationOutcome::SchedulerValidationMalformed:
    return "malformed";
  }
  return "unknown";
}

} // namespace

void
MempoolCoordinatorRpc::record_submitted_vetted(size_t count)
{
  std::scoped_lock lock(telemetry_mutex_);
  commit_window_telemetry_.submitted_vetted += count;
}

void
MempoolCoordinatorRpc::record_submitted_unvetted(size_t count)
{
  std::scoped_lock lock(telemetry_mutex_);
  commit_window_telemetry_.submitted_unvetted += count;
}

void
MempoolCoordinatorRpc::record_nomination_attempt(
    const NominationTelemetry &telemetry)
{
  std::scoped_lock lock(telemetry_mutex_);
  commit_window_telemetry_.nomination = telemetry;
}

void
MempoolCoordinatorRpc::record_validation_attempt(
    ValidationTelemetry telemetry) noexcept
{
  std::scoped_lock lock(telemetry_mutex_);
  try {
    commit_window_telemetry_.validations.push_back(std::move(telemetry));
  } catch (std::exception &e) {
    // Telemetry collection is best-effort and must not escape deferred cleanup.
    info("could not collect telemetry due to exception thrown: {}", e.what());
  }
}

auto
MempoolCoordinatorRpc::snapshot_commit_window_telemetry() const
    -> CommitWindowTelemetry
{
  std::scoped_lock lock(telemetry_mutex_);
  return commit_window_telemetry_;
}

void
MempoolCoordinatorRpc::reset_commit_window_telemetry()
{
  std::scoped_lock lock(telemetry_mutex_);
  commit_window_telemetry_ = {};
}

x23::Async<void>
MempoolCoordinatorRpc::schedulercoord_submit_unvetted(
    ParallelSignedTransactionBatch batch)
{
  std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration delta = end - last_;
  auto delta_raw = std::chrono::duration_cast<std::chrono::milliseconds>(delta);
  int64_t delta_count = delta_raw.count();

  ElapsedTimer timer;
  double last_stage_ts = 0.0;
  auto record_stage = [&](double &stage_out) {
    const double now = timer.elapsed();
    stage_out = now - last_stage_ts;
    last_stage_ts = now;
  };
  double stage_deserialize_batch = 0.0;
  double stage_auth_verify = 0.0;
  double stage_wait_commit_lock_before_add = 0.0;
  double stage_filter_invalid = 0.0;
  double stage_add_txs = 0.0;

  if (scheduler_->size() >= kMempoolTxSizeLimit) {
    info(
        "submit_unvetted rejected txs as the mempool has too many pending txs");
    co_return;
  }

  auto txs = std::make_shared<SignedTransactionBatch>();
  if (!(co_await parallel_batch_to_t(batch, *txs))) {
    info("submit_unvetted rejected malformed transaction batch");
    co_return;
  }
  size_t num_submitted_txs = txs->size();
  record_submitted_unvetted(num_submitted_txs);
  record_stage(stage_deserialize_batch);

  {
    // [scheduler_->size()] may have changed since we last checked it above, so
    // we read it again here.
    size_t scheduler_size = scheduler_->size();
    if (scheduler_size >= kMempoolTxSizeLimit) {
      info("submit_unvetted rejected txs as the mempool has too many pending "
           "txs");
      co_return;
    }
    size_t num_slots_left = scheduler_size - kMempoolTxSizeLimit;
    if (num_slots_left < txs->size()) {
      // The submitted txs will bring the mempool beyond its pending tx capacity
      // limit, so we discard the extra txs.
      info("submit_unvetted received {} txs, but only {} will fit in the "
           "mempool. discarding {} txs.",
           num_submitted_txs, num_slots_left, txs->size() - num_slots_left);
      txs->resize(num_slots_left);
    }
  }

  x23::CoroGuard cg;
  co_await cg(commit_lock_);

  const BlockHeight verify_height = scheduler_->current_block();
  info("submit_unvetted will check if {} txs are valid for height {}",
       txs->size(), verify_height);
  AuthErrorCodeBatch auth_ret =
      co_await auth_client_->authmempool_verify_transactions(verify_height,
                                                             *txs);
  record_stage(stage_auth_verify);

  x23::Vec<size_t> to_remove_indices;
  for (size_t i = 0; i < auth_ret.size(); i++) {
    const AuthErrorCode &code = auth_ret[i];
    if (code != AuthErrorCode::kAuthSuccess) {
      to_remove_indices.push_back(i);
    }
  }
  info("{} txs were invalid", to_remove_indices.size());
  if (!to_remove_indices.empty()) {
    remove_indices(/*remove_from=*/*txs, /*indices=*/to_remove_indices);
  }
  record_stage(stage_filter_invalid);

  pending_detached_coroutines_.fetch_add(1, std::memory_order_acquire);
  [](MempoolCoordinatorRpc &coord,
     std::shared_ptr<SignedTransactionBatch> txs) -> x23::Detached {
    // We serialize and send the response in a detached coroutine so that we can
    // release [commit_lock_] as soon as [add_txs()] is finished below.
    // Serializing and sending the response takes longer than [add_txs()] and
    // does not require holding [commit_lock_].

    // TODO(jhumphri): We could avoid serializing [txs] and reuse [batch] if we
    // do not remove any txs above. I am not sure this is the common case
    // though.
    ElapsedTimer timer;
    ParallelBatch response;
    co_await t_to_parallel_batch(*txs, response);
    co_await coord.coord_client_->coordscheduler_notify_vetted(response);
    coord.pending_detached_coroutines_.fetch_sub(1, std::memory_order_release);
    info("submit_unvetted() prepare response and notify = {}", timer.elapsed());
  }(*this, txs);

  // Careful: it is tempting to overlap the overhead of
  // [authmempool_verify_transactions()] with that of [scheduler_->add_txs()]
  // (and then after remove any unverified txs from the scheduler). But this
  // would make the txs visible to nominate() and validate() before the txs have
  // been verified.
  co_await scheduler_->add_txs(*txs);
  record_stage(stage_add_txs);

  info("submit_unvetted() stages: delta={}, deserialize_batch={:.6f}, "
       "auth_verify={:.6f}, "
       "wait_commit_lock_before_add={:.6f}, filter_invalid={:.6f}, "
       "add_txs={:.6f}, total={:.6f}, scheduler_size={}, submitted_txs = {}, "
       "vetted_txs={}",
       delta_count, stage_deserialize_batch, stage_auth_verify,
       stage_wait_commit_lock_before_add, stage_filter_invalid, stage_add_txs,
       timer.elapsed(), scheduler_->size(), num_submitted_txs, txs->size());
  co_return;
}

x23::Async<void>
MempoolCoordinatorRpc::schedulercoord_submit_vetted(
    ValidatorID validator_id, ParallelSignedTransactionBatch batch)
{
  if (scheduler_->size() >= kMempoolTxSizeLimit) {
    info("submit_vetted rejected txs as the mempool has too many pending txs");
    co_return;
  }

  SignedTransactionBatch txs;
  if (!(co_await parallel_batch_to_t(batch, txs))) {
    info("submit_vetted rejected malformed transaction batch");
    co_return;
  }
  num_submitted_txs = txs.size();
  record_submitted_vetted(num_submitted_txs);
  info("submit_vetted, {} txs", txs.size());

  {
    // [scheduler_->size()] may have changed since we last checked it above, so
    // we read it again here.
    size_t scheduler_size = scheduler_->size();
    if (scheduler_size >= kMempoolTxSizeLimit) {
      info("submit_vetted rejected txs as the mempool has too many pending "
           "txs");
      co_return;
    }
    size_t num_slots_left = scheduler_size - kMempoolTxSizeLimit;
    if (num_slots_left < txs.size()) {
      // The submitted txs will bring the mempool beyond its pending tx capacity
      // limit, so we discard the extra txs.
      info("submit_vetted received {} txs, but only {} will fit in the "
           "mempool. discarding {} txs.",
           num_submitted_txs, num_slots_left, txs.size() - num_slots_left);
      txs.resize(num_slots_left);
    }
  }

  x23::CoroGuard cg;
  co_await cg(commit_lock_);
  info("submit_vetted, got lock. will now add txs");
  co_await scheduler_->add_txs(txs);
  info("submit_vetted, finished adding txs. will now return");
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
MempoolCoordinatorRpc::commit_snapshot(BlockID block_id, BlockHash block_hash,
                                       SHA256Hash events_hash)
{
  co_return co_await db_sched_client_->dbsched_commit_snapshot(block_id,
                                                               block_hash,
                                                               events_hash);
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
  x23::Vec<x23::Async<void>> handles;
  handles.reserve(exec_clients_.size());
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    handles.push_back(finish_block(block_id, /*executor_idx=*/i));
  }
  for (x23::Async<void> &handle : handles) {
    co_await handle;
  }
}

SHA256Hash
MempoolCoordinatorRpc::get_txset_hash(
    const x23::XdrVec<SignedTransactionHash> &hashes)
{
  MerkleTriePrefix trie(/*allocator_type=*/Allocator<Node>::Type::kBump);
  trie.set_should_hash_nodes_on_each_op(false);
  WorkerPool &worker_pool = WorkerPool::get_default_pool();
  const size_t num_threads = available_parallelism();
  std::vector<std::future<void>> futures;
  const std::vector<ChunkRange> chunks =
      make_chunks(hashes.size(), num_threads);
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
  trie.generate_all_hashes(num_threads);
  return trie.get_root_hash();
}

x23::Async<void>
MempoolCoordinatorRpc::propose_batch(
    BlockID block_id, x23::span<const SignedTransaction> txs,
    x23::XdrVec<NondeterministicProposalData> &transaction_nominations,
    x23::span<bool> ret_chunk, size_t executor_idx)
{
  using TxProjection =
      x23::ProjectedView<SignedTransaction, &SignedTransaction::tx>;
  ExecProposeBatchResult res =
      co_await exec_clients_[executor_idx].executorsched_propose_batch(
          block_id, TxProjection(txs));
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
  const size_t kNumBigChunks = available_parallelism();
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
  ElapsedTimer timer;
  double last_stage_ts = 0.0;
  auto record_stage = [&](double &stage_out) {
    const double now = timer.elapsed();
    stage_out = now - last_stage_ts;
    last_stage_ts = now;
  };
  double stage_spawn_snapshot = 0.0;
  double stage_open_event_block = 0.0;
  double stage_spawn_open_proposal_blocks = 0.0;
  double stage_await_snapshot = 0.0;
  double stage_await_open_proposal_blocks = 0.0;
  double stage_cleanup_failures = 0.0;
  double stage_finalize = 0.0;

  // Create snapshot.
  x23::Async<DbSchedResult> create_snapshot_handler =
      create_snapshot(/*prev_block_id=*/last_committed_block_.id,
                      cache.block_id,
                      BlockExecutionMode::kBlockExecutionModeProposal);
  record_stage(stage_spawn_snapshot);

  // Open block (on event server). [open_proposal_block()] below ultimately
  // causes RPCs to be sent to the server event server. When the event server
  // receives those RPCs, it expects the block to already be opened. Thus, we
  // [co_await eventsched_open_block()] below before doing
  // [open_proposal_block()] to prevent a race.
  CHECK_EQ(co_await eventsched_open_block(cache.block_id), RPCError::kRPCNone);
  record_stage(stage_open_event_block);

  // Open proposal blocks.
  x23::Vec<x23::Async<ExecResult>> open_proposal_block_handlers;
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    open_proposal_block_handlers.push_back(
        open_proposal_block(cache.block_id,
                            /*executor_idx=*/i));
  }
  record_stage(stage_spawn_open_proposal_blocks);

  // [co_await] the block initialization RPCs.
  DbSchedResult create_snapshot_handler_ret = co_await create_snapshot_handler;
  record_stage(stage_await_snapshot);

  x23::Vec<ExecResult> open_proposal_block_handler_ret;
  bool open_proposal_block_success = true;
  for (x23::Async<ExecResult> &handler : open_proposal_block_handlers) {
    open_proposal_block_handler_ret.push_back(co_await handler);
    if (open_proposal_block_handler_ret.back() != ExecResult::ExecResultOK) {
      open_proposal_block_success = false;
    }
  }
  record_stage(stage_await_open_proposal_blocks);

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
    co_await finish_all_executors(cache.block_id);
    record_stage(stage_cleanup_failures);
    record_stage(stage_finalize);

    info("initialize_block() stages: spawn_snapshot={:.6f}, "
         "open_event_block={:.6f}, spawn_open_proposal_blocks={:.6f}, "
         "await_snapshot={:.6f}, await_open_proposal_blocks={:.6f}, "
         "cleanup_failures={:.6f}, finalize={:.6f}, total={:.6f}, "
         "snapshot_success={}, open_proposal_success={}, executors={}",
         stage_spawn_snapshot, stage_open_event_block,
         stage_spawn_open_proposal_blocks, stage_await_snapshot,
         stage_await_open_proposal_blocks, stage_cleanup_failures,
         stage_finalize, timer.elapsed(),
         create_snapshot_handler_ret == DbSchedResult::DbSchedResultOK,
         open_proposal_block_success, open_proposal_block_handlers.size());

    co_return false;
  }
  record_stage(stage_finalize);
  info("initialize_block() stages: spawn_snapshot={:.6f}, "
       "open_event_block={:.6f}, spawn_open_proposal_blocks={:.6f}, "
       "await_snapshot={:.6f}, await_open_proposal_blocks={:.6f}, "
       "cleanup_failures={:.6f}, finalize={:.6f}, total={:.6f}, "
       "snapshot_success={}, open_proposal_success={}, executors={}",
       stage_spawn_snapshot, stage_open_event_block,
       stage_spawn_open_proposal_blocks, stage_await_snapshot,
       stage_await_open_proposal_blocks, stage_cleanup_failures, stage_finalize,
       timer.elapsed(),
       create_snapshot_handler_ret == DbSchedResult::DbSchedResultOK,
       open_proposal_block_success, open_proposal_block_handlers.size());
  co_return true;
}

x23::Async<void>
MempoolCoordinatorRpc::schedule(PendingBlock &cache)
{
  ElapsedTimer timer;
  double last_stage_ts = 0.0;
  auto record_stage = [&](double &stage_out) {
    const double now = timer.elapsed();
    stage_out = now - last_stage_ts;
    last_stage_ts = now;
  };
  double stage_lock_wait = 0.0;
  double stage_schedule_loop = 0.0;
  double stage_await_batches = 0.0;

  x23::CoroGuard cg;
  co_await cg(scheduler_->global_lock_);
  record_stage(stage_lock_wait);

  x23::Vec<x23::Async<void>> handles;
  handles.reserve(kNumSchedulerIterations);
  x23::Vec<size_t> handle_end_indices;
  handle_end_indices.reserve(kNumSchedulerIterations);
  cache.transaction_nomination_chunks.clear();
  cache.transaction_nomination_chunks.resize(kNumSchedulerIterations, {});
  size_t num_scheduled = 0;
  double total_schedule_time = 0.0;
  for (size_t i = 0; i < kNumSchedulerIterations; i++) {
    if (i > 0) {
      // The first call to [schedule()] is done in [schedulercoord_nominate()]
      // since we want to overlap it with the block initialization RPCs.
      ElapsedTimer timer;
      scheduler_->schedule(kMaxBlockSize / kNumSchedulerIterations,
                           cache.tx_hashes, cache.txs);
      total_schedule_time += timer.elapsed();
      info("schedule() iteration took {}, scheduler size is {}",
           timer.elapsed(), scheduler_->size());
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
    {
      ElapsedTimer timer;
      handles.push_back(
          propose_batch_in_chunks(cache.block_id, txs_span,
                                  cache.transaction_nomination_chunks[i],
                                  ret_span));
      handle_end_indices.push_back(cache.tx_hashes.size());
      info("schedule() propose_batch_in_chunks() took {}", timer.elapsed());
    }
  }
  info("schedule(), total schedule time is {}", total_schedule_time);
  record_stage(stage_schedule_loop);

  cache.failed_txs.clear();
  size_t num_tx_outcomes_processed = 0;
  CHECK(cache.result_tx_nominations.empty());
  for (size_t handle_idx = 0; handle_idx < handles.size(); handle_idx++) {
    co_await handles[handle_idx];
    cache.result_tx_nominations +=
        cache.transaction_nomination_chunks[handle_idx];

    // TODO(jhumphri): Start adding the txs to the merkle trie here.
    for (size_t tx_idx = num_tx_outcomes_processed;
         tx_idx < handle_end_indices[handle_idx]; tx_idx++) {
      if (cache.statuses[tx_idx]) {
        cache.result_tx_hashes.push_back(cache.tx_hashes[tx_idx]);
      }
      else {
        cache.failed_txs.push_back(cache.txs[tx_idx]);
      }
    }
    num_tx_outcomes_processed = handle_end_indices[handle_idx];
  }
  CHECK_EQ(num_tx_outcomes_processed, cache.tx_hashes.size());
  CHECK_EQ(cache.result_tx_hashes.size(), cache.result_tx_nominations.size());
  record_stage(stage_await_batches);

  info("schedule() stages: lock_wait={:.6f}, schedule_loop={:.6f}, "
       "await_batches={:.6f}, total={:.6f}, handles={}, tx_hashes={}",
       stage_lock_wait, stage_schedule_loop, stage_await_batches,
       timer.elapsed(), handles.size(), cache.tx_hashes.size());

  co_return;
}

x23::Async<void>
MempoolCoordinatorRpc::finish_nominating_block(const DbTip &db_tip,
                                               PendingBlock &cache)
{
  ElapsedTimer timer;
  double last_stage_ts = 0.0;
  auto record_stage = [&](double &stage_out) {
    const double now = timer.elapsed();
    stage_out = now - last_stage_ts;
    last_stage_ts = now;
  };
  double stage_spawn_finish_calls = 0.0;
  double stage_generate_txset_hash = 0.0;
  double stage_serialize_nomination = 0.0;
  double stage_close_snapshot = 0.0;
  double stage_event_close_block = 0.0;
  double stage_generate_block_hash = 0.0;
  double stage_await_finish_calls = 0.0;

  // Gracefully shut down executor work and send the nomination chunks
  // (including the final block hash) back to the coordinator.
  x23::Async<void> finish_block_handle = finish_all_executors(cache.block_id);
  record_stage(stage_spawn_finish_calls);

  x23::Async<DbBlockResult> close_snapshot_handle =
      close_snapshot(cache.block_id);
  x23::Async<EventBlockResult> event_close_block_handle =
      eventsched_close_block(cache.block_id, db_tip.height + 1);

  SHA256Hash txset_hash = get_txset_hash(cache.result_tx_hashes);
  record_stage(stage_generate_txset_hash);
  {
    Nomination &nomination =
        cache.nomination_result.nominated_block().nomination;
    nomination.transactions_hash = {};
    nomination.transactions_nondeterministic_data = {};
    co_await t_to_parallel_batch(cache.result_tx_hashes,
                                 nomination.transactions_hash);
    co_await t_to_parallel_batch(cache.result_tx_nominations,
                                 nomination.transactions_nondeterministic_data);
    co_await t_to_parallel_batch(cache.txs,
                                 cache.nomination_result.nominated_block().txs);
    record_stage(stage_serialize_nomination);
  }
  {
    DbBlockResult result = co_await close_snapshot_handle;
    CHECK_EQ(result.result(), DbSchedResult::DbSchedResultOK);
    cache.db_tip.db_hash = result.db_hash();
    record_stage(stage_close_snapshot);
  }
  {
    EventBlockResult result = co_await event_close_block_handle;
    CHECK_EQ(result.error(), RPCError::kRPCNone);
    cache.db_tip.events_hash = result.root_hash();
    record_stage(stage_event_close_block);
  }
  {
    Nomination &nomination =
        cache.nomination_result.nominated_block().nomination;
    nomination.block_height = db_tip.height + 1;

    BlockHeader header{.network_id = network_id_,
                       .block_height = nomination.block_height,
                       .txset_hash = txset_hash,
                       .db_hash = cache.db_tip.db_hash,
                       .events_hash = cache.db_tip.events_hash,
                       .prev_block_hash = db_tip.block_hash};
    nomination.block_hash = sha256(header);
    nomination.prev_block_hash = db_tip.block_hash;
    record_stage(stage_generate_block_hash);
  }

  co_await finish_block_handle;
  record_stage(stage_await_finish_calls);

  info("finish_nominating_block() stages: spawn_finish_calls={:.6f}, "
       "generate_txset_hash={:.6f}, serialize_nomination={:.6f}, "
       "close_snapshot={:.6f}, event_close_block={:.6f}, "
       "generate_block_hash={:.6f}, await_finish_calls={:.6f}, "
       "total={:.6f}",
       stage_spawn_finish_calls, stage_generate_txset_hash,
       stage_serialize_nomination, stage_close_snapshot,
       stage_event_close_block, stage_generate_block_hash,
       stage_await_finish_calls, timer.elapsed());
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
  ElapsedTimer timer;
  double last_stage_ts = 0.0;
  auto record_stage = [&](double &stage_out) {
    const double now = timer.elapsed();
    stage_out = now - last_stage_ts;
    last_stage_ts = now;
  };
  double stage_await_commit_snapshot = 0.0;

  x23::Async<DbBlockResult> commit_snapshot_handle =
      commit_snapshot(cache.block_id, cache.db_tip.block_hash,
                      cache.db_tip.events_hash);
  {
    DbBlockResult result = co_await commit_snapshot_handle;
    CHECK_EQ(result.result(), DbSchedResult::DbSchedResultOK);
  }
  record_stage(stage_await_commit_snapshot);

  info("commit_block() stages: await_commit_snapshot={:.6f}, total={:.6f}, "
       "block_id={}",
       stage_await_commit_snapshot, timer.elapsed(), cache.block_id);
}

x23::Async<SchedulerNominationResult>
MempoolCoordinatorRpc::schedulercoord_nominate(const DbTip db_tip)
{
  ElapsedTimer timer;
  NominationTelemetry nomination_telemetry;
  nomination_telemetry.attempted = true;
  double last_stage_ts = 0.0;
  auto record_stage = [&](double &stage_out) {
    const double now = timer.elapsed();
    stage_out = now - last_stage_ts;
    last_stage_ts = now;
  };
  x23::Defer record_nomination_stats([&] {
    nomination_telemetry.duration = timer.elapsed();
    record_nomination_attempt(nomination_telemetry);
  });
  double stage_precheck = 0.0;
  double stage_wait_transfers = 0.0;
  double stage_wait_auth_data = 0.0;
  double stage_clear_a = 0.0;
  double stage_clear_b = 0.0;
  double stage_clear_c = 0.0;
  double stage_clear_d = 0.0;
  double stage_clear = 0.0;
  double stage_initialize_overlap = 0.0;
  double stage_schedule_remaining = 0.0;
  double stage_add_failed_txs = 0.0;
  double stage_finish_nominating = 0.0;
  double stage_finalize_result = 0.0;
  std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration delta = end - last_;
  // NOLINTNEXTLINE(modernize-use-auto):
  std::chrono::milliseconds delta_raw =
      std::chrono::duration_cast<std::chrono::milliseconds>(delta);
  int64_t delta_count = delta_raw.count();
  info("NOMINATE, delta = {}", delta_count);

  x23::CoroGuard cg;
  co_await cg(nominated_block_cache_lock_);

  // TODO(jhumphri): chuckd does not know the current [db_hash] or
  // [events_hash], so we only check the other fields in [db_tip]. Beyond this,
  // I feel like it is incorrect to have [schedulercoord_nominate()] have the
  // [DbTip] type as a parameter--I feel like it should only take the latest
  // *block* hash as a parameter, and maybe we could also have the latest block
  // height as a parameter if we wanted to have checks that everything is
  // consistent.
  //
  // Similarly, for [schedulercoord_validate()], I think we should only take
  // [nomination] as a parameter, which includes the latest block hash and
  // height as fields. We should remove [db_tip].
  //
  // I am going to discuss this with Petar and address accordingly.
  // if (db_tip != last_committed_block_) {
  if (db_tip.height != last_committed_block_.height ||
      db_tip.id != last_committed_block_.id ||
      db_tip.block_hash != last_committed_block_.block_hash) {
    last_ = std::chrono::steady_clock::now();
    info("schedulercoord_nominate ret");
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }
  if (nominated_block_cache_.block_id) {
    // A block was nominated by this node and not yet committed or abandoned.
    // [schedulercoord_commit_block()] needs to be called before this node can
    // nominate another block.
    last_ = std::chrono::steady_clock::now();
    info("schedulercoord_nominate ret");
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }
  record_stage(stage_precheck);

  // Wait for hogdb to notify us of all HOG transfers that occurred in the
  // previous block. These transfers affect account balances, which in turn
  // affect scheduling priorities for accounts, so we need to wait for the
  // transfer data for the last committed block before scheduling the next
  // block.
  co_await wait_for_transfer_data(db_tip.height);
  record_stage(stage_wait_transfers);
  co_await wait_for_auth_data(db_tip.height);
  record_stage(stage_wait_auth_data);

  nominated_block_cache_.block_id = generate_block_id();
  // We place the block txs and tx hashes into these vectors. We mark these
  // vectors static and clear them on each call to [nominate()] so to avoid
  // memory allocation/free overhead.
  x23::XdrVec<SignedTransaction> tmp = std::move(nominated_block_cache_.txs);
  [](x23::XdrVec<SignedTransaction> to_clear) -> x23::Detached {
    co_await default_coropoll().pool_.bg();
    to_clear.clear();
  }(std::move(tmp));

  nominated_block_cache_.txs = {};
  nominated_block_cache_.txs.reserve(kMaxBlockSize);
  record_stage(stage_clear_a);
  nominated_block_cache_.tx_hashes.clear();
  record_stage(stage_clear_b);
  nominated_block_cache_.result_tx_hashes.clear();
  record_stage(stage_clear_c);
  nominated_block_cache_.result_tx_nominations.clear();
  record_stage(stage_clear_d);
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
  record_stage(stage_clear);

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
    scheduler_->start_pending_nomination(nominated_block_cache_.block_id);
    scheduler_->schedule(kMaxBlockSize / kNumSchedulerIterations,
                         nominated_block_cache_.tx_hashes,
                         nominated_block_cache_.txs);
    info("schedule() gave us {} txs", nominated_block_cache_.tx_hashes.size());
    if (!nominated_block_cache_.tx_hashes.empty()) {
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
    {
      x23::CoroGuard cg;
      co_await cg(scheduler_->global_lock_);
      scheduler_->abandon_pending_nomination(nominated_block_cache_.block_id);
    }
    last_ = std::chrono::steady_clock::now();
    info("schedulercoord_nominate ret");
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }
  record_stage(stage_initialize_overlap);

  // Schedule the block and send the txs to the executors.
  co_await schedule(nominated_block_cache_);
  record_stage(stage_schedule_remaining);
  info("schedule() gave us {} txs, scheduler size is {}",
       nominated_block_cache_.tx_hashes.size(), scheduler_->size());

  // Process the transaction outcomes.
  if (!nominated_block_cache_.failed_txs.empty()) {
    co_await scheduler_->add_txs(nominated_block_cache_.failed_txs);
  }
  record_stage(stage_add_failed_txs);

  co_await finish_nominating_block(db_tip, nominated_block_cache_);
  record_stage(stage_finish_nominating);

  // We set [nominated_block_cache_.nomination_result.outcome()] to
  // [SchedulerNominationOutcome::SchedulerNominationOK] above. See the comments
  // there.
  nominated_block_cache_.nomination_result.nominated_block().block_id =
      nominated_block_cache_.block_id;
  nomination_telemetry.succeeded = true;
  nomination_telemetry.block_id = nominated_block_cache_.block_id;
  nomination_telemetry.block_height =
      nominated_block_cache_.nomination_result.nominated_block()
          .nomination.block_height;
  nomination_telemetry.scheduled_tx_count =
      nominated_block_cache_.tx_hashes.size();
  nomination_telemetry.nominated_tx_count =
      nominated_block_cache_.result_tx_hashes.size();
  nomination_telemetry.failed_tx_count =
      nominated_block_cache_.failed_txs.size();
  nominated_block_cache_.db_tip = DbTip{
      .height = nominated_block_cache_.nomination_result.nominated_block()
                    .nomination.block_height,
      .block_hash = nominated_block_cache_.nomination_result.nominated_block()
                        .nomination.block_hash,
      .events_hash = nominated_block_cache_.db_tip.events_hash,
      .db_hash = nominated_block_cache_.db_tip.db_hash,
      .id =
          nominated_block_cache_.nomination_result.nominated_block().block_id};
  record_stage(stage_finalize_result);
  info("nominate() stages: precheck={:.6f}, wait_transfers={:.6f}, "
       "wait_auth_data={:.6f}, clear_a={:.6f}, clear_b={:.6f}, clear_c={:.6f}, "
       "clear_d={:.6f}, clear={:.6f}, initialize_overlap={:.6f}, "
       "schedule_remaining={:.6f}, add_failed_txs={:.6f}, "
       "finish_nominating={:.6f}, finalize_result={:.6f}, total={:.6f}, num tx "
       "hashes {}, num tx nominations {}",
       stage_precheck, stage_wait_transfers, stage_wait_auth_data,
       stage_clear_a, stage_clear_b, stage_clear_c, stage_clear_d, stage_clear,
       stage_initialize_overlap, stage_schedule_remaining, stage_add_failed_txs,
       stage_finish_nominating, stage_finalize_result, timer.elapsed(),
       nominated_block_cache_.result_tx_hashes.size(),
       nominated_block_cache_.result_tx_nominations.size());
  last_ = std::chrono::steady_clock::now();
  info("schedulercoord_nominate ret");
  co_return nominated_block_cache_.nomination_result;
}

x23::Async<bool>
MempoolCoordinatorRpc::validate_batch_chunk(
    BlockID block_id, x23::span<const ProposedTransaction> txs,
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

x23::Async<MempoolCoordinatorRpc::PreparedValidationChunk>
MempoolCoordinatorRpc::prepare_validation_chunk(
    x23::span<const SignedTransactionHash> tx_hashes,
    x23::span<const NondeterministicProposalData> tx_nominations)
{
  PreparedValidationChunk prepared;
  prepared.proposed_txs.resize(tx_hashes.size());
  x23::Vec<bool> tx_found(tx_hashes.size(), false);

  {
    ElapsedTimer lookup_timer;
    const std::vector<ChunkRange> lookup_chunks =
        make_chunks(tx_hashes.size(), available_parallelism());
    x23::Vec<x23::Async<void>> lookup_handles;
    lookup_handles.reserve(lookup_chunks.size());

    for (const ChunkRange &lookup_chunk : lookup_chunks) {
      lookup_handles.push_back(
          [](const ChunkRange *lookup_chunk,
             x23::span<const SignedTransactionHash> tx_hashes,
             x23::span<const NondeterministicProposalData> tx_nominations,
             x23::Vec<ProposedTransaction> &proposed_txs,
             x23::Vec<bool> &tx_found,
             std::shared_ptr<Scheduler> scheduler) noexcept
              -> x23::Async<void> {
            co_await default_coropoll().pool_.bg();

            for (size_t i = lookup_chunk->start; i < lookup_chunk->end; i++) {
              std::shared_ptr<const SignedTransaction> found_tx =
                  scheduler->find_tx(tx_hashes[i]);
              if (!found_tx) {
                continue;
              }
              proposed_txs[i] = {.signed_tx = *found_tx,
                                 .nondeterministic_proposal_data =
                                     tx_nominations[i]};
              tx_found[i] = true;
            }
            co_return;
          }(&lookup_chunk, tx_hashes, tx_nominations, prepared.proposed_txs,
              tx_found, scheduler_));
    }
    for (x23::Async<void> &handle : lookup_handles) {
      co_await handle;
    }
    prepared.lookup_duration = lookup_timer.elapsed();
    info("this lookup took {}", prepared.lookup_duration);

    ElapsedTimer build_timer;
    for (size_t i = 0; i < tx_found.size(); i++) {
      if (!tx_found[i]) {
        prepared.missing_tx_hashes.push_back(tx_hashes[i]);
      }
    }
    prepared.build_duration = build_timer.elapsed();
    info("this build took {}", prepared.build_duration);
  }

  if (!prepared.missing_tx_hashes.empty()) {
    prepared.proposed_txs.clear();
  }

  co_return prepared;
}

x23::Async<MempoolCoordinatorRpc::PreparedAndValidatedChunk>
MempoolCoordinatorRpc::prepare_and_validate_chunk(
    BlockID block_id, x23::span<const SignedTransactionHash> tx_hashes,
    x23::span<const NondeterministicProposalData> tx_nominations)
{
  co_await default_coropoll().pool_.bg();

  ElapsedTimer a;
  PreparedValidationChunk prepared =
      co_await prepare_validation_chunk(tx_hashes, tx_nominations);

  info("prepare_validation_chunk took {}", a.elapsed());
  PreparedAndValidatedChunk ret{
      .missing_tx_hashes = std::move(prepared.missing_tx_hashes),
      .lookup_duration = prepared.lookup_duration,
      .build_duration = prepared.build_duration,
      .validate_duration = 0.0,
      .validated = true,
  };
  if (!ret.missing_tx_hashes.empty()) {
    co_return ret;
  }

  ElapsedTimer validate_timer;
  ret.validated = co_await validate_batch(block_id, prepared.proposed_txs);
  ret.validate_duration = validate_timer.elapsed();
  info("validate_batch took {}", validate_timer.elapsed());
  ElapsedTimer last;
  ret.txs.reserve(prepared.proposed_txs.size());
  for (ProposedTransaction &proposed_tx : prepared.proposed_txs) {
    ret.txs.push_back(std::move(proposed_tx.signed_tx));
  }
  info("last bit of tx work took {}", last.elapsed());
  co_return ret;
}

x23::Async<bool>
MempoolCoordinatorRpc::validate_batch(BlockID block_id,
                                      const x23::Vec<ProposedTransaction> &txs)
{
  // We split the [txs] into [kNumBigChunks] chunks and then send each chunk
  // from a different coroutine. Each coroutine then further splits the chunk
  // into smaller RPC-sized chunks and stripes those requests across executors.
  const size_t kNumBigChunks = available_parallelism();
  const std::vector<ChunkRange> large_chunks =
      make_chunks(txs.size(), kNumBigChunks);

  x23::Vec<x23::Async<bool>> handles;
  handles.reserve(large_chunks.size());

  for (const ChunkRange &chunk : large_chunks) {
    x23::span<const ProposedTransaction> tx_chunk(txs.begin() + chunk.start,
                                                  txs.begin() + chunk.end);
    handles.push_back(
        multiplex_large_validation_chunk_across_executors(block_id, tx_chunk));
  }

  bool validated = true;
  for (x23::Async<bool> &handle : handles) {
    validated = (co_await handle) && validated;
  }
  co_return validated;
}

x23::Async<bool>
MempoolCoordinatorRpc::multiplex_large_validation_chunk_across_executors(
    BlockID block_id, x23::span<const ProposedTransaction> txs)
{
  co_await default_coropoll().pool_.bg();

  const std::vector<ChunkRange> chunks =
      make_chunks_for_max_size(txs.size(), kChunkSize);

  x23::Vec<x23::Async<bool>> handles;
  handles.reserve(chunks.size());

  for (size_t i = 0; i < chunks.size(); i++) {
    const ChunkRange &chunk = chunks[i];
    x23::span<const ProposedTransaction> tx_chunk(txs.begin() + chunk.start,
                                                  txs.begin() + chunk.end);
    handles.push_back(
        validate_batch_chunk(block_id, tx_chunk,
                             /*executor_idx=*/i % exec_clients_.size()));
  }

  bool validated = true;
  for (x23::Async<bool> &handle : handles) {
    validated = (co_await handle) && validated;
  }
  co_return validated;
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
  ElapsedTimer timer;
  double last_stage_ts = 0.0;
  auto record_stage = [&](double &stage_out) {
    const double now = timer.elapsed();
    stage_out = now - last_stage_ts;
    last_stage_ts = now;
  };
  auto accumulate_stage = [&](double &stage_out) {
    const double now = timer.elapsed();
    stage_out += now - last_stage_ts;
    last_stage_ts = now;
  };
  double stage_wait_validation_gate = 0.0;
  double stage_precheck = 0.0;
  double stage_deserialize_nomination_data = 0.0;
  double stage_deserialize_tx_hashes = 0.0;
  double stage_wait_transfers = 0.0;
  double stage_wait_auth_data = 0.0;
  double stage_create_snapshot = 0.0;
  double stage_open_event_block = 0.0;
  double stage_open_validation_blocks = 0.0;
  double stage_lookup_txs = 0.0;
  double stage_block_execution = 0.0;
  double stage_close_snapshot = 0.0;
  double stage_close_event_block = 0.0;
  double stage_verify_block_hash = 0.0;
  double stage_finish_executors = 0.0;
  double stage_serialize_result = 0.0;
  double stage_cache_validated_block = 0.0;
  double stage_finalize = 0.0;
  double stage_reserve_cache = 0.0;
  ValidationTelemetry validation_telemetry{.block_id = 0,
                                           .block_height =
                                               nomination.block_height,
                                           .outcome = "unknown",
                                           .duration = 0.0,
                                           .tx_count = 0};
  x23::Defer record_validation_stats([&] {
    validation_telemetry.duration = timer.elapsed();
    record_validation_attempt(std::move(validation_telemetry));
  });
  info("schedulercoord_validate, validating block for height = {}",
       nomination.block_height);

  while (true) {
    while (!(co_await increment_num_validations_in_progress())) {
      co_await wait_for_validation_gate_open();
    }
    accumulate_stage(stage_wait_validation_gate);

    if (nomination.block_height < last_committed_block_.height + 1) {
      // This block is too old to validate.
      co_await decrement_num_validations_in_progress();
      validation_telemetry.outcome = validation_outcome_str(
          SchedulerValidationOutcome::SchedulerValidationTooLateToValidate);
      info("schedulercoord_validate ret");
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationTooLateToValidate);
    }
    else if (nomination.block_height > last_committed_block_.height + 1) {
      // This block is ahead of the current block in the mempool. Thus, we need
      // to wait for the mempool to catch up before verifying this block.
      co_await decrement_num_validations_in_progress();
      // If we want to validate a block at height [x], we need to wait for the
      // block at height [x - 1] to commit.
      co_await wait_for_block_commit(nomination.block_height - 1);
      // Rather than break out of the loop here, we instead continue to the next
      // iteration of the loop. This prevents a race where a different block at
      // height [x] is committed before we are able to wake up, increment the
      // [num_validations_in_progress_] futex, and exit the loop. If that race
      // could occur, then we would fail the CHECK below. Instead, we go to the
      // next iteration, increment the [num_validations_in_progress_] futex, and
      // then either break out of the loop or we return
      // [SchedulerValidationTooLateToValidate] if a different block at height x
      // has already been committed.
    }
    else {
      break;
    }
  }
  CHECK_EQ(nomination.block_height, last_committed_block_.height + 1);

  if (last_committed_block_.block_hash != nomination.prev_block_hash) {
    co_await decrement_num_validations_in_progress();
    validation_telemetry.outcome = validation_outcome_str(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
    info("schedulercoord_validate ret");
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }
  record_stage(stage_precheck);

  // TODO(jhumphri): Need to ensure that the node that proposed the block is
  // following the expected scheduling policy.
  x23::XdrVec<NondeterministicProposalData> tx_nominations;
  PendingBlock cache{};
  {
    // We call [reserve] here to avoid racing with accesses to [cache.tx_hashes]
    // in [parallel_batch_to_t()] below.
    cache.txs.reserve(kMaxBlockSize);
    cache.tx_hashes.reserve(kMaxBlockSize);
    cache.result_tx_hashes.reserve(kMaxBlockSize);
    cache.result_tx_nominations.reserve(kMaxBlockSize);
    record_stage(stage_reserve_cache);

    x23::Async<bool> tx_nondeterministic_data_handle =
        parallel_batch_to_t(nomination.transactions_nondeterministic_data,
                            tx_nominations);
    x23::Async<bool> tx_hash_handle =
        parallel_batch_to_t(nomination.transactions_hash, cache.tx_hashes);

    const bool decoded_tx_nominations =
        co_await tx_nondeterministic_data_handle;
    record_stage(stage_deserialize_nomination_data);

    const bool decoded_tx_hashes = co_await tx_hash_handle;
    record_stage(stage_deserialize_tx_hashes);

    if (!decoded_tx_nominations || !decoded_tx_hashes) {
      co_await decrement_num_validations_in_progress();
      validation_telemetry.outcome = validation_outcome_str(
          SchedulerValidationOutcome::SchedulerValidationMalformed);
      info("schedulercoord_validate rejected malformed nomination batch");
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationMalformed);
    }

    if (cache.tx_hashes.size() != tx_nominations.size()) {
      co_await decrement_num_validations_in_progress();
      validation_telemetry.outcome = validation_outcome_str(
          SchedulerValidationOutcome::SchedulerValidationMalformed);
      info("schedulercoord_validate rejected nomination with {} tx hashes and "
           "{} nondeterministic data entries",
           cache.tx_hashes.size(), tx_nominations.size());
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationMalformed);
    }

    // TODO(jhumphri): Remember that we need to pre-size the vectors in [cache]
    // correctly when we use the helper functions above to send txs to the
    // executors.
    cache.block_id = generate_block_id();
    validation_telemetry.block_id = cache.block_id;
    validation_telemetry.tx_count = cache.tx_hashes.size();
  }

  co_await wait_for_transfer_data(nomination.block_height - 1);
  record_stage(stage_wait_transfers);
  co_await wait_for_auth_data(nomination.block_height - 1);
  record_stage(stage_wait_auth_data);

  DbSchedResult snapshot_result = co_await create_snapshot(
      /*prev_block_id=*/db_tip.id, cache.block_id,
      BlockExecutionMode::kBlockExecutionModeValidation);
  record_stage(stage_create_snapshot);
  if (snapshot_result != DbSchedResult::DbSchedResultOK) {
    co_await decrement_num_validations_in_progress();
    if (snapshot_result == DbSchedResult::DbSchedResultInvalidParentId) {
      validation_telemetry.outcome = validation_outcome_str(
          SchedulerValidationOutcome::SchedulerValidationUnknownParentBlock);
      info("schedulercoord_validate ret");
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationUnknownParentBlock);
    }
    info("schedulercoord_validate ret");
    CHECK_EQ(snapshot_result, DbSchedResult::DbSchedResultError);
    validation_telemetry.outcome = validation_outcome_str(
        SchedulerValidationOutcome::SchedulerValidationDbFailed);
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationDbFailed);
  }
  CHECK_EQ(co_await eventsched_open_block(cache.block_id), RPCError::kRPCNone);
  record_stage(stage_open_event_block);

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
  record_stage(stage_open_validation_blocks);
  if (!open_validation_block_success) {
    co_await finish_all_executors(cache.block_id);
    co_await abandon_block(cache);
    co_await decrement_num_validations_in_progress();
    validation_telemetry.outcome = validation_outcome_str(
        SchedulerValidationOutcome::SchedulerValidationExecutorFailed);
    info("schedulercoord_validate ret");
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationExecutorFailed);
  }

  info("schedulercoord_validate here");
  // [nomination] includes the the tx hashes and the nondeterministic proposal
  // data. We need to use these tx hashes to look up the vetted txs in
  // [vetted_txs_]. Assuming we find all of the txs, we then combine them with
  // the nondeterminstic proposal data to form a [ProposedTransaction], which
  // is what the executors expect to receive.
  x23::XdrVec<SignedTransactionHash> missing_tx_hashes;
  bool validated = true;
  SHA256Hash txset_hash;
  {
    const std::vector<ChunkRange> validation_chunks =
        make_chunks_for_max_size(cache.tx_hashes.size(),
                                 kNumTxsPerNominationChunk);
    x23::Vec<x23::Async<PreparedAndValidatedChunk>> chunk_handles;
    chunk_handles.reserve(validation_chunks.size());

    for (const ChunkRange &chunk : validation_chunks) {
      x23::span<const SignedTransactionHash> tx_hashes_span(
          cache.tx_hashes.begin() + chunk.start,
          cache.tx_hashes.begin() + chunk.end);
      x23::span<const NondeterministicProposalData> tx_nominations_span(
          tx_nominations.begin() + chunk.start,
          tx_nominations.begin() + chunk.end);
      chunk_handles.push_back(prepare_and_validate_chunk(cache.block_id,
                                                         tx_hashes_span,
                                                         tx_nominations_span));
    }

    // Compute the txset hash here so that we can overlap its compute overhead
    // with that of the block execution.
    txset_hash = get_txset_hash(cache.tx_hashes);

    x23::Vec<PreparedAndValidatedChunk> chunk_results;
    chunk_results.reserve(chunk_handles.size());
    for (x23::Async<PreparedAndValidatedChunk> &handle : chunk_handles) {
      chunk_results.push_back(co_await handle);
    }
    for (PreparedAndValidatedChunk &chunk_result : chunk_results) {
      stage_lookup_txs += chunk_result.lookup_duration;
      validated = chunk_result.validated && validated;
      missing_tx_hashes += chunk_result.missing_tx_hashes;
      cache.txs += chunk_result.txs;
    }
    if (!chunk_results.empty()) {
      stage_lookup_txs /= chunk_results.size();
    }

    if (!missing_tx_hashes.empty()) {
      co_await finish_all_executors(cache.block_id);
      co_await abandon_block(cache);
      co_await decrement_num_validations_in_progress();
      validation_telemetry.outcome = validation_outcome_str(
          SchedulerValidationOutcome::SchedulerValidationMissingTxs);

      SchedulerValidationResult ret(
          SchedulerValidationOutcome::SchedulerValidationMissingTxs);
      ret.missing_tx_hashes() = missing_tx_hashes;
      info("schedulercoord_validate ret");
      co_return ret;
    }
    CHECK_EQ(cache.txs.size(), tx_nominations.size());
  }
  record_stage(stage_block_execution);

  if (!validated) {
    co_await finish_all_executors(cache.block_id);
    co_await abandon_block(cache);
    co_await decrement_num_validations_in_progress();
    validation_telemetry.outcome = validation_outcome_str(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
    info("schedulercoord_validate ret");
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }
  x23::Async<void> finish_block_handle = finish_all_executors(cache.block_id);
  {
    x23::Async<DbBlockResult> close_snapshot_handle =
        close_snapshot(cache.block_id);
    x23::Async<EventBlockResult> event_close_block_handle =
        eventsched_close_block(cache.block_id, scheduler_->current_block() + 1);

    {
      DbBlockResult result = co_await close_snapshot_handle;
      CHECK_EQ(result.result(), DbSchedResult::DbSchedResultOK);
      cache.db_tip.db_hash = result.db_hash();
    }
    record_stage(stage_close_snapshot);

    {
      EventBlockResult result = co_await event_close_block_handle;
      CHECK_EQ(result.error(), RPCError::kRPCNone);
      cache.db_tip.events_hash = result.root_hash();
    }
    record_stage(stage_close_event_block);
  }

  BlockHeader header{.network_id = network_id_,
                     .block_height = nomination.block_height,
                     .txset_hash = txset_hash,
                     .db_hash = cache.db_tip.db_hash,
                     .events_hash = cache.db_tip.events_hash,
                     .prev_block_hash = nomination.prev_block_hash};
  SHA256Hash block_hash = sha256(header);
  record_stage(stage_verify_block_hash);
  if (block_hash != nomination.block_hash) {
    co_await finish_block_handle;
    co_await decrement_num_validations_in_progress();
    validation_telemetry.outcome = validation_outcome_str(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
    info("schedulercoord_validate ret");
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }
  co_await finish_block_handle;
  record_stage(stage_finish_executors);

  cache.db_tip = DbTip{.height = nomination.block_height,
                       .block_hash = nomination.block_hash,
                       .events_hash = cache.db_tip.events_hash,
                       .db_hash = cache.db_tip.db_hash,
                       .id = cache.block_id};
  SchedulerValidationResult result(
      SchedulerValidationOutcome::SchedulerValidationOK);
  result.block().db_tip = cache.db_tip;
  result.block().signed_transactions = {};
  co_await t_to_parallel_batch(cache.txs, result.block().signed_transactions);
  record_stage(stage_serialize_result);

  const BlockID block_id = cache.block_id;
  {
    x23::CoroGuard cache_cg;
    co_await cache_cg(validated_block_cache_lock_);
    CHECK(!validated_block_cache_.contains(cache.block_id));
    validated_block_cache_[block_id] = std::move(cache);
  }
  record_stage(stage_cache_validated_block);
  co_await decrement_num_validations_in_progress();
  record_stage(stage_finalize);
  validation_telemetry.outcome =
      validation_outcome_str(SchedulerValidationOutcome::SchedulerValidationOK);
  info("validate() stages: wait_validation_gate={:.6f}, precheck={:.6f}, "
       "reserve_cache={:.6f}, "
       "deserialize_nomination_data={:.6f}, deserialize_tx_hashes={:.6f}, "
       "wait_transfers={:.6f}, wait_auth_data={:.6f}, "
       "create_snapshot={:.6f}, open_event_block={:.6f}, "
       "open_validation_blocks={:.6f}, lookup_txs={:.6f}, "
       "block_execution={:.6f}, "
       "close_snapshot={:.6f}, close_event_block={:.6f}, "
       "verify_block_hash={:.6f}, finish_executors={:.6f}, "
       "serialize_result={:.6f}, cache_validated_block={:.6f}, "
       "finalize={:.6f}, total={:.6f}, block_id={}, block_height={}, txs={}",
       stage_wait_validation_gate, stage_precheck, stage_reserve_cache,
       stage_deserialize_nomination_data, stage_deserialize_tx_hashes,
       stage_wait_transfers, stage_wait_auth_data, stage_create_snapshot,
       stage_open_event_block, stage_open_validation_blocks, stage_lookup_txs,
       stage_block_execution, stage_close_snapshot, stage_close_event_block,
       stage_verify_block_hash, stage_finish_executors, stage_serialize_result,
       stage_cache_validated_block, stage_finalize, timer.elapsed(), block_id,
       nomination.block_height, validation_telemetry.tx_count);
  info("schedulercoord_validate ret");
  co_return result;
}

// TODO(jhumphri): We need to update the scheduler hog ages for the txs in the
// validation block if the validation block is committed.
// TODO(jhumphri): We need to undo the changes to the scheduler hog ages for the
// txs in the nomination block if the nomination block is not committed.
x23::Async<SchedulerCommitOutcome>
MempoolCoordinatorRpc::schedulercoord_commit_block(BlockID block_id)
{
  const std::chrono::steady_clock::time_point commit_started_at =
      std::chrono::steady_clock::now();
  {
    std::chrono::steady_clock::duration delta =
        commit_started_at - last_commit_;
    auto delta_raw =
        std::chrono::duration_cast<std::chrono::milliseconds>(delta);
    info("schedulercoord_commit_block, committing block for height = {}, last "
         "committed block height = {}, delta between end of last commit and "
         "this "
         "commit = {}",
         last_committed_block_.height + 1, last_committed_block_.height,
         delta_raw.count());
  }
  ElapsedTimer timer;
  double last_stage_ts = 0.0;
  auto record_stage = [&](double &stage_out) {
    const double now = timer.elapsed();
    stage_out = now - last_stage_ts;
    last_stage_ts = now;
  };
  double stage_wait_commit_lock = 0.0;
  double stage_wait_validations = 0.0;
  double stage_precheck_validations = 0.0;
  double stage_wait_nominated_lock = 0.0;
  double stage_precheck_block_presence = 0.0;
  double stage_build_handles = 0.0;
  double stage_compute_reenqueue_set = 0.0;
  double stage_remove_from_scheduler = 0.0;
  double stage_await_handles = 0.0;
  double stage_write_average_hog_ages = 0.0;
  double stage_finalize = 0.0;
  auto emit_commit_stats = [&](const char *outcome, size_t handles_size,
                               size_t reenqueue_size,
                               size_t remove_from_scheduler_size) {
    info("schedulercoord_commit_block() stages: "
         "wait_commit_lock={:.6f}, wait_validations={:.6f}, "
         "precheck_validations={:.6f}, wait_nominated_lock={:.6f}, "
         "precheck_block_presence={:.6f}, build_handles={:.6f}, "
         "compute_reenqueue_set={:.6f}, remove_from_scheduler={:.6f}, "
         "await_handles={:.6f}, write_average_hog_ages={:.6f}, "
         "finalize={:.6f}, total={:.6f}, outcome={}, "
         "block_id={}, handles={}, reenqueue_size={}, remove_size={}",
         stage_wait_commit_lock, stage_wait_validations,
         stage_precheck_validations, stage_wait_nominated_lock,
         stage_precheck_block_presence, stage_build_handles,
         stage_compute_reenqueue_set, stage_remove_from_scheduler,
         stage_await_handles, stage_write_average_hog_ages, stage_finalize,
         timer.elapsed(), outcome, block_id, handles_size, reenqueue_size,
         remove_from_scheduler_size);
  };

  std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration delta = end - last_;
  // NOLINTNEXTLINE(modernize-use-auto):
  std::chrono::milliseconds delta_raw =
      std::chrono::duration_cast<std::chrono::milliseconds>(delta);
  int64_t delta_count = delta_raw.count();
  info("schedulercoord_commit_block, delta = {}", delta_count);

  co_await close_validation_gate();
  x23::Defer reopen_validation_gate([this] { open_validation_gate(); });
  record_stage(stage_wait_validations);
  record_stage(stage_precheck_validations);

  x23::CoroGuard nominate_cg;
  co_await nominate_cg(nominated_block_cache_lock_);
  record_stage(stage_wait_nominated_lock);

  if ((!nominated_block_cache_.block_id ||
       nominated_block_cache_.block_id != block_id) &&
      !validated_block_cache_.contains(block_id)) {
    record_stage(stage_precheck_block_presence);
    emit_commit_stats("block_not_found", /*handles_size=*/0,
                      /*reenqueue_size=*/0,
                      /*remove_from_scheduler_size=*/0);
    // A block with ID [block_id] was neither nominated nor validated, so it
    // cannot be committed.
    last_ = std::chrono::steady_clock::now();
    info("schedulercoord_commit_block ret");
    co_return SchedulerCommitOutcome::SchedulerCommitBlockNotFound;
  }
  record_stage(stage_precheck_block_presence);

  x23::CoroGuard commit_cg;
  co_await commit_cg(commit_lock_);
  record_stage(stage_wait_commit_lock);

  x23::Vec<x23::Async<void>> commit_handles;
  commit_handles.reserve(validated_block_cache_.size());
  x23::Vec<x23::Async<DbSchedResult>> snapshot_handles;
  snapshot_handles.reserve(validated_block_cache_.size());
  if (nominated_block_cache_.block_id &&
      nominated_block_cache_.block_id == block_id) {
    // We are committing the block that we nominated.
    commit_handles.push_back(commit_block(nominated_block_cache_));
    last_committed_block_ = nominated_block_cache_.db_tip;
    info("setting last_committed_block_ height to {}",
         nominated_block_cache_.db_tip.height);
  }
  else {
    // We are committing a block that we validated.
    commit_handles.push_back(commit_block(validated_block_cache_[block_id]));
    if (nominated_block_cache_.block_id) {
      // We nominated a block, so we need to abandon the block.
      snapshot_handles.push_back(
          abandon_snapshot(nominated_block_cache_.block_id));
    }
    last_committed_block_ = validated_block_cache_[block_id].db_tip;
    info("setting last_committed_block_ height to {}",
         validated_block_cache_[block_id].db_tip.height);
  }
  for (const auto &[_, cache] : validated_block_cache_) {
    if (cache.block_id != block_id) {
      snapshot_handles.push_back(abandon_snapshot(cache.block_id));
    }
  }
  record_stage(stage_build_handles);

  // We need to do two things:
  // 1. If the nominated block is not the committed block, we need to return
  // the nominated block's txs to the scheduler.
  //
  // 2. Next, we need to remove the committed block's txs from the scheduler.
  // Note that some of these txs may overlap with the txs in the nominated
  // block, so we can optimize this by only returning to the scheduler the
  // txs in the nominated block that are not in the committed block. Then we
  // can remove all of the committed block's txs from the scheduler.
  //
  // Importantly, this logic must only look at txs that actually made it into
  // the nominated block. [nominated_block_cache_.tx_hashes] includes every tx
  // we attempted during scheduling, including txs that later failed proposal
  // and were re-added to the scheduler via [failed_txs].
  static std::unordered_set<SignedTransactionHash, ByteArrayHasher>
      return_to_runqueue;
  return_to_runqueue.clear();
  const x23::XdrVec<SignedTransactionHash> *remove_from_scheduler = nullptr;
  if (nominated_block_cache_.block_id &&
      nominated_block_cache_.block_id == block_id) {
    // The committed block is the block we nominated.
    remove_from_scheduler = &nominated_block_cache_.result_tx_hashes;
  }
  else {
    // The committed block is a block we validated.
    if (nominated_block_cache_.block_id &&
        nominated_block_cache_.block_id != block_id) {
      // Return the nominated block's txs to the runqueue so they can be
      // scheduled again.
      return_to_runqueue.insert(nominated_block_cache_.result_tx_hashes.begin(),
                                nominated_block_cache_.result_tx_hashes.end());
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
  record_stage(stage_compute_reenqueue_set);

  CHECK_CONST_PTR(remove_from_scheduler);
  scheduler_->reenqueue_txs(return_to_runqueue.cbegin(),
                            return_to_runqueue.cend());
  const size_t remove_from_scheduler_size = remove_from_scheduler->size();
  co_await scheduler_->remove_from_scheduler(
      x23::span<const SignedTransactionHash>(*remove_from_scheduler));
  if (nominated_block_cache_.block_id &&
      nominated_block_cache_.block_id == block_id) {
    x23::CoroGuard cg;
    co_await cg(scheduler_->global_lock_);
    scheduler_->commit_pending_nomination(block_id);
  }
  else {
    if (nominated_block_cache_.block_id) {
      x23::CoroGuard cg;
      co_await cg(scheduler_->global_lock_);
      scheduler_->abandon_pending_nomination(nominated_block_cache_.block_id);
    }
    const PendingBlock *committed_cache = nullptr;
    auto it = validated_block_cache_.find(block_id);
    CHECK(it != validated_block_cache_.end());
    committed_cache = &it->second;
    CHECK_CONST_PTR(committed_cache);
    // [apply_committed_block_hog_ages] acquires [scheduler_->global_lock_] for
    // us, so we do not need to do it here.
    co_await scheduler_->apply_committed_block_hog_ages(
        committed_cache->txs, committed_cache->db_tip.height);
  }
  record_stage(stage_remove_from_scheduler);

  // We write the hog ages to the database down here because we need to commit
  // the hog ages above first ([scheduler_->commit_pending_nomination()] and
  // [scheduler_->apply_committed_block_hog_ages]).
  scheduler_->write_all_average_hog_ages_to_db();
  record_stage(stage_write_average_hog_ages);

  for (x23::Async<void> &handle : commit_handles) {
    co_await handle;
  }
  for (x23::Async<DbSchedResult> &handle : snapshot_handles) {
    CHECK_EQ(co_await handle, DbSchedResult::DbSchedResultOK);
  }
  record_stage(stage_await_handles);

  // Be careful to not do [nominated_block_cache_ = {}]. We pre-allocate
  // elements in the vectors in [nominated_block_cache_], so if we clear those
  // vectors, we will experience segfaults because we will later attempt to
  // read/write indices that do not exist.
  nominated_block_cache_.block_id = 0;
  validated_block_cache_.clear();

  scheduler_->increment_current_block();
  record_stage(stage_finalize);
  emit_commit_stats("ok", commit_handles.size() + snapshot_handles.size(),
                    return_to_runqueue.size(), remove_from_scheduler_size);

  const CommitWindowTelemetry commit_window_stats =
      snapshot_commit_window_telemetry();
  std::ostringstream validation_summary;
  for (size_t i = 0; i < commit_window_stats.validations.size(); i++) {
    const ValidationTelemetry &validation = commit_window_stats.validations[i];
    if (i != 0) {
      validation_summary << "; ";
    }
    validation_summary << "{idx=" << i << ", block_id=" << validation.block_id
                       << ", height=" << validation.block_height
                       << ", outcome=" << validation.outcome
                       << ", duration=" << validation.duration
                       << ", txs=" << validation.tx_count << "}";
  }
  const size_t scheduler_size = scheduler_->size();
  const size_t total_submitted = commit_window_stats.submitted_vetted +
                                 commit_window_stats.submitted_unvetted;
  const auto since_last_commit_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(commit_started_at -
                                                            last_commit_);
  if (commit_window_stats.nomination.attempted) {
    info("commit summary: height={}, block_id={}, since_last_commit_ms={}, "
         "commit_duration={:.6f}, stage_wait_commit_lock={:.6f}, "
         "stage_wait_validations={:.6f}, stage_precheck_validations={:.6f}, "
         "stage_wait_nominated_lock={:.6f}, "
         "stage_precheck_block_presence={:.6f}, stage_build_handles={:.6f}, "
         "stage_compute_reenqueue_set={:.6f}, "
         "stage_remove_from_scheduler={:.6f}, stage_await_handles={:.6f}, "
         "stage_write_average_hog_ages={:.6f}, stage_finalize={:.6f}, "
         "scheduler_size={}, submitted_total={}, "
         "submitted_vetted={}, submitted_unvetted={}, nomination={{succeeded="
         "{}, block_id={}, height={}, duration={:.6f}, scheduled_txs={}, "
         "nominated_txs={}, failed_txs={}}}, validations={}, "
         "validation_details=[{}]",
         last_committed_block_.height, block_id, since_last_commit_ms.count(),
         timer.elapsed(), stage_wait_commit_lock, stage_wait_validations,
         stage_precheck_validations, stage_wait_nominated_lock,
         stage_precheck_block_presence, stage_build_handles,
         stage_compute_reenqueue_set, stage_remove_from_scheduler,
         stage_await_handles, stage_write_average_hog_ages, stage_finalize,
         scheduler_size, total_submitted, commit_window_stats.submitted_vetted,
         commit_window_stats.submitted_unvetted,
         commit_window_stats.nomination.succeeded,
         commit_window_stats.nomination.block_id,
         commit_window_stats.nomination.block_height,
         commit_window_stats.nomination.duration,
         commit_window_stats.nomination.scheduled_tx_count,
         commit_window_stats.nomination.nominated_tx_count,
         commit_window_stats.nomination.failed_tx_count,
         commit_window_stats.validations.size(), validation_summary.str());
  }
  else {
    info("commit summary: height={}, block_id={}, since_last_commit_ms={}, "
         "commit_duration={:.6f}, stage_wait_commit_lock={:.6f}, "
         "stage_wait_validations={:.6f}, stage_precheck_validations={:.6f}, "
         "stage_wait_nominated_lock={:.6f}, "
         "stage_precheck_block_presence={:.6f}, stage_build_handles={:.6f}, "
         "stage_compute_reenqueue_set={:.6f}, "
         "stage_remove_from_scheduler={:.6f}, stage_await_handles={:.6f}, "
         "stage_write_average_hog_ages={:.6f}, stage_finalize={:.6f}, "
         "scheduler_size={}, submitted_total={}, "
         "submitted_vetted={}, submitted_unvetted={}, nomination=none, "
         "validations={}, validation_details=[{}]",
         last_committed_block_.height, block_id, since_last_commit_ms.count(),
         timer.elapsed(), stage_wait_commit_lock, stage_wait_validations,
         stage_precheck_validations, stage_wait_nominated_lock,
         stage_precheck_block_presence, stage_build_handles,
         stage_compute_reenqueue_set, stage_remove_from_scheduler,
         stage_await_handles, stage_write_average_hog_ages, stage_finalize,
         scheduler_size, total_submitted, commit_window_stats.submitted_vetted,
         commit_window_stats.submitted_unvetted,
         commit_window_stats.validations.size(), validation_summary.str());
  }
  reset_commit_window_telemetry();
  last_ = std::chrono::steady_clock::now();
  info("schedulercoord_commit_block ret");
  last_commit_ = std::chrono::steady_clock::now();
  co_return SchedulerCommitOutcome::SchedulerCommitOK;
}

// TODO(jhumphri): After a commit, I think I need to check that
// [auth_data_changed] was called before any subsequent calls to nominate() or
// validate(). Also be sure to make sure this works with the genesis block.
// I do not need to check this in add_txs--this is all kept in sync by the
// [auth_data_changed] callback.
x23::Async<void>
MempoolCoordinatorRpc::auth_data_changed(BlockHeight height,
                                         KeyChangeBatch key_changes)
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
  AuthErrorCodeBatch auth_ret =
      co_await auth_client_->authmempool_verify_transactions(
          scheduler_->current_block(), txs);
  x23::Vec<SignedTransactionHash> to_remove;
  for (size_t i = 0; i < auth_ret.size(); i++) {
    const AuthErrorCode &code = auth_ret[i];
    if (code != AuthErrorCode::kAuthSuccess) {
      to_remove.push_back(tx_hashes[i]);
    }
  }
  co_await scheduler_->remove_from_scheduler(to_remove);
  auth_data_height_.store(height);
  auth_data_height_.broadcast(&default_threadpool());
}

} // namespace hogd
