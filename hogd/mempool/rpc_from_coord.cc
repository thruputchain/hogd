#include "hogd/mempool/rpc_from_coord.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
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
#include "hogd/mempool/shared.h"
#include "nomination.hh"
#include "parallel.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/corosync.h"
#include "x23/dag.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

enum class CommitMetricsOutcome { kOk, kBlockNotFound };

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

bool
tx_succeeded(const ExecResult &r)
{
  return r == ExecResult::ExecResultTxDeterministicError ||
         r == ExecResult::ExecResultOK ||
         r == ExecResult::ExecResultTxRootFrameReverted;
}

bool
tx_failed(const ExecResult &r)
{
  return !tx_succeeded(r);
}

template<typename T>
x23::DagDeps
dag_deps_from_nodes(const x23::Vec<x23::DagNode<T>> &nodes)
{
  x23::DagDeps deps;
  deps.ids.reserve(nodes.size());
  for (const x23::DagNode<T> &node : nodes) {
    deps.ids.push_back(node.id());
  }
  return deps;
}

x23::DagDeps
merge_dag_deps(x23::DagDeps a, const x23::DagDeps &b)
{
  a.ids.reserve(a.ids.size() + b.ids.size());
  a.ids.insert(a.ids.end(), std::make_move_iterator(b.ids.begin()),
               std::make_move_iterator(b.ids.end()));
  return a;
}

template<typename T>
void
append_serialized_segment(x23::span<const T> objs, ParallelBatch &parallel)
{
  CHECK_LE(objs.size(), std::numeric_limits<unsigned int>::max());
  x23::XdrVec<std::uint8_t> xdr;
  xdr.reserve(objs.size() * sizeof(T) * 3);
  for (const T &obj : objs) {
    xdr += x23::serialize(obj);
  }
  CHECK_LE(xdr.size(), std::numeric_limits<unsigned int>::max());
  parallel.segment_tokens.push_back(static_cast<unsigned int>(objs.size()));
  parallel.segment_bytes.push_back(static_cast<unsigned int>(xdr.size()));
  parallel.xdr += xdr;
}

template<typename T>
void
append_selected_serialized_segment(x23::span<const T> objs,
                                   x23::span<const bool> use,
                                   ParallelBatch &parallel)
{
  CHECK_EQ(objs.size(), use.size());
  x23::XdrVec<std::uint8_t> xdr;
  xdr.reserve(objs.size() * sizeof(T) * 3);
  size_t num_tokens = 0;
  for (size_t i = 0; i < objs.size(); i++) {
    if (use[i]) {
      xdr += x23::serialize(objs[i]);
      num_tokens++;
    }
  }
  CHECK_LE(num_tokens, std::numeric_limits<unsigned int>::max());
  CHECK_LE(xdr.size(), std::numeric_limits<unsigned int>::max());
  parallel.segment_tokens.push_back(static_cast<unsigned int>(num_tokens));
  parallel.segment_bytes.push_back(static_cast<unsigned int>(xdr.size()));
  parallel.xdr += xdr;
}

// clang tidy complains if the body of [x23::Defer()] can throw an exception, so
// we wrap the body in this try/catch block.
template<typename F>
void
record_metric_cleanup(F &&f) noexcept
{
  try {
    std::forward<F>(f)();
  } catch (...) {
    std::terminate();
  }
}

} // namespace

void
MempoolCoordinatorRpc::record_submitted_vetted(size_t count)
{
  MempoolMetrics::instance().submitted_vetted_tx_count.increment(count);
}

void
MempoolCoordinatorRpc::record_submitted_unvetted(size_t count)
{
  MempoolMetrics::instance().submitted_unvetted_tx_count.increment(count);
}

void
MempoolCoordinatorRpc::record_nomination_attempt(
    const NominationTelemetry &telemetry)
{
  MempoolMetrics::instance().last_nomination_duration_ms.record(
      mempool_duration_ms(telemetry.duration));
  MempoolMetrics::instance().nomination_attempt_count.increment();
  if (telemetry.succeeded) {
    MempoolMetrics::instance().nomination_success_count.increment();
  }
  else {
    MempoolMetrics::instance().nomination_error_count.increment();
  }
  MempoolMetrics::instance().nomination_scheduled_tx_count.increment(
      telemetry.scheduled_tx_count);
  MempoolMetrics::instance().nomination_nominated_tx_count.increment(
      telemetry.nominated_tx_count);
  MempoolMetrics::instance().nomination_failed_tx_count.increment(
      telemetry.failed_tx_count);
}

void
MempoolCoordinatorRpc::record_validation_attempt(
    SchedulerValidationOutcome outcome, double duration, size_t tx_count)
{
  MempoolMetrics::instance().last_validation_duration_ms.record(
      mempool_duration_ms(duration));
  MempoolMetrics::instance().validation_attempt_count.increment();
  MempoolMetrics::instance().validation_tx_count.increment(tx_count);
  switch (outcome) {
  case SchedulerValidationOutcome::SchedulerValidationOK:
    MempoolMetrics::instance().validation_ok_count.increment();
    break;
  case SchedulerValidationOutcome::SchedulerValidationInvalid:
    MempoolMetrics::instance().validation_invalid_count.increment();
    break;
  case SchedulerValidationOutcome::SchedulerValidationTooLateToValidate:
    MempoolMetrics::instance().validation_too_late_count.increment();
    break;
  case SchedulerValidationOutcome::SchedulerValidationMissingTxs:
    MempoolMetrics::instance().validation_missing_txs_count.increment();
    break;
  case SchedulerValidationOutcome::SchedulerValidationUnknownParentBlock:
    MempoolMetrics::instance().validation_unknown_parent_count.increment();
    break;
  case SchedulerValidationOutcome::SchedulerValidationDbFailed:
    MempoolMetrics::instance().validation_db_failed_count.increment();
    break;
  case SchedulerValidationOutcome::SchedulerValidationExecutorFailed:
    MempoolMetrics::instance().validation_executor_failed_count.increment();
    break;
  case SchedulerValidationOutcome::SchedulerValidationMalformed:
    MempoolMetrics::instance().validation_malformed_count.increment();
    break;
  }
}

x23::Async<void>
MempoolCoordinatorRpc::schedulercoord_submit_unvetted(
    ParallelSignedTransactionBatch batch)
{
  CHECK(ready());

  ElapsedTimer timer;
  x23::Defer record_duration([&] {
    record_metric_cleanup([&] {
      MempoolMetrics::instance().last_submit_unvetted_duration_ms.record(
          mempool_duration_ms(timer.elapsed()));
    });
  });

  if (scheduler_->size() >= mempool_tx_size_limit_) {
    MempoolMetrics::instance().rejected_capacity_rpc_count.increment();
    info(
        "submit_unvetted rejected txs as the mempool has too many pending txs");
    co_return;
  }

  auto txs = std::make_shared<SignedTransactionBatch>();
  if (!(co_await parallel_batch_to_t(batch, *txs))) {
    MempoolMetrics::instance()
        .rejected_malformed_unvetted_rpc_count.increment();
    info("submit_unvetted rejected malformed transaction batch");
    co_return;
  }
  size_t num_submitted_txs = txs->size();
  record_submitted_unvetted(num_submitted_txs);

  x23::CoroGuard cg;
  co_await cg(commit_lock_);

  {
    // [scheduler_->size()] may have changed since we last checked it above, so
    // we read it again here. Importantly, we do this while holding
    // [commit_lock_] to ensure that concurrent RPCs cannot push us beyond the
    // mempool tx capacity limit.
    ssize_t scheduler_size = scheduler_->size();
    if (scheduler_size >= mempool_tx_size_limit_) {
      MempoolMetrics::instance().rejected_capacity_rpc_count.increment();
      info("submit_unvetted rejected txs as the mempool has too many pending "
           "txs");
      co_return;
    }
    ssize_t num_slots_left = mempool_tx_size_limit_ - scheduler_size;
    CHECK_GE(num_slots_left, 0L);
    if (std::cmp_less(num_slots_left, txs->size())) {
      // The submitted txs will bring the mempool beyond its pending tx capacity
      // limit, so we discard the extra txs.
      //
      // We CHECK above that [num_slots_left] is >= 0, so it is safe to cast it
      // to size_t.
      const auto resize_to = static_cast<size_t>(num_slots_left);
      const size_t num_discarded = txs->size() - resize_to;

      MempoolMetrics::instance().discarded_capacity_tx_count.increment(
          num_discarded);
      txs->resize(resize_to);
    }
  }

  const BlockHeight verify_height = scheduler_->current_block();
  AuthErrorCodeBatch auth_ret =
      co_await auth_client_->authmempool_verify_transactions(verify_height,
                                                             *txs);

  x23::Vec<size_t> to_remove_indices;
  for (size_t i = 0; i < auth_ret.size(); i++) {
    const AuthErrorCode &code = auth_ret[i];
    if (code != AuthErrorCode::kAuthSuccess) {
      mark_tx_failed_authentication(sha256((*txs)[i].tx));
      to_remove_indices.push_back(i);
    }
  }
  MempoolMetrics::instance().auth_failed_tx_count.increment(
      to_remove_indices.size());
  if (!to_remove_indices.empty()) {
    remove_indices(/*remove_from=*/*txs, /*indices=*/to_remove_indices);
  }

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
    ParallelBatch response;
    co_await t_to_parallel_batch(*txs, response);
    co_await coord.coord_client_->coordscheduler_notify_vetted(response);
    coord.pending_detached_coroutines_.fetch_sub(1, std::memory_order_release);
  }(*this, txs);

  // Careful: it is tempting to overlap the overhead of
  // [authmempool_verify_transactions()] with that of [scheduler_->add_txs()]
  // (and then after remove any unverified txs from the scheduler). But this
  // would make the txs visible to nominate() and validate() before the txs have
  // been verified.
  co_await scheduler_->add_txs(*txs);
  MempoolMetrics::instance().accepted_unvetted_tx_count.increment(txs->size());
  co_return;
}

x23::Async<void>
MempoolCoordinatorRpc::schedulercoord_submit_vetted(
    ValidatorID validator_id, ParallelSignedTransactionBatch batch)
{
  CHECK(ready());
  ElapsedTimer timer;
  x23::Defer record_duration([&] {
    record_metric_cleanup([&] {
      MempoolMetrics::instance().last_submit_vetted_duration_ms.record(
          mempool_duration_ms(timer.elapsed()));
    });
  });

  // We deliberately do not discard *vetted* txs even if the mempool is at
  // capacity because we want to avoid this deadlock:
  // 1. The mempool receives a block to validate.
  // 2. The mempool is missing txs from that block, so it requests them from the
  //    coordinator.
  // 3. The coordinator sends the requested txs to the mempool via this current
  //    RPC call ([schedulercoord_submit_vetted()]).
  // 4. If the mempool is at capacity and we were to check for that here, the
  //    mempool would drop the txs.
  // 5. Now the block can never be validated by this mempool because the
  //    requested txs are never enqueued in the mempool. Note that if the
  //    validated block becomes committed on other nodes, this mempool will
  //    never be able to advance to future blocks in the chain. And furthermore,
  //    this mempool will never be asked to nominate a block, so it will be
  //    unable to process and erase the currently enqueued txs, so it will never
  //    free up capacity.
  SignedTransactionBatch txs;
  if (!(co_await parallel_batch_to_t(batch, txs))) {
    MempoolMetrics::instance().rejected_malformed_vetted_rpc_count.increment();
    info("submit_vetted rejected malformed transaction batch");
    co_return;
  }
  size_t num_submitted_txs = txs.size();
  record_submitted_vetted(num_submitted_txs);

  x23::CoroGuard cg;
  co_await cg(commit_lock_);

  const BlockHeight verify_height = scheduler_->current_block();
  AuthErrorCodeBatch auth_ret =
      co_await auth_client_->authmempool_verify_transactions(verify_height,
                                                             txs);

  x23::Vec<size_t> to_remove_indices;
  for (size_t i = 0; i < auth_ret.size(); i++) {
    const AuthErrorCode &code = auth_ret[i];
    if (code != AuthErrorCode::kAuthSuccess) {
      mark_tx_failed_authentication(sha256(txs[i].tx));
      to_remove_indices.push_back(i);
    }
  }
  MempoolMetrics::instance().auth_failed_tx_count.increment(
      to_remove_indices.size());
  if (!to_remove_indices.empty()) {
    remove_indices(/*remove_from=*/txs, /*indices=*/to_remove_indices);
  }

  co_await scheduler_->add_txs(txs);
  MempoolMetrics::instance().accepted_vetted_tx_count.increment(txs.size());
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

x23::Async<RPCError>
MempoolCoordinatorRpc::eventsched_commit_block(BlockID block_id)
{
  co_return co_await event_sched_client_->eventsched_commit_block(block_id);
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
    const x23::XdrVec<SignedTransactionHash> &hashes, bool log_timings)
{
  ElapsedTimer timer;
  MerkleTriePrefix trie(/*allocator_type=*/Allocator<Node>::Type::kBump);
  WorkerPool &worker_pool = WorkerPool::get_default_pool();
  const size_t num_threads = available_parallelism();
  std::vector<std::future<void>> futures;
  const std::vector<ChunkRange> chunks =
      make_chunks(hashes.size(), num_threads);
  futures.reserve(chunks.size());
  const double setup_elapsed = timer.elapsed();

  for (const ChunkRange &chunk : chunks) {
    futures.push_back(worker_pool.submit([chunk, &trie, &hashes] {
      for (size_t j = chunk.start; j < chunk.end; j++) {
        // We add each tx hash to the Merkle trie as a key with an empty value.
        // So in essence, we are using the Merkle trie as a set.
        CHECK(trie.insert(HexArray(hashes[j]), std::span<const std::byte>()));
      }
    }));
  }
  const double submit_elapsed = timer.elapsed() - setup_elapsed;
  for (std::future<void> &future : futures) {
    future.get();
  }
  const double insert_elapsed =
      timer.elapsed() - setup_elapsed - submit_elapsed;
  trie.generate_all_hashes(num_threads);
  const double generate_elapsed =
      timer.elapsed() - setup_elapsed - submit_elapsed - insert_elapsed;
  SHA256Hash root_hash = trie.get_root_hash();
  const double root_elapsed = timer.elapsed() - setup_elapsed - submit_elapsed -
                              insert_elapsed - generate_elapsed;
  const double total_elapsed = timer.elapsed();
  if (log_timings) {
    info("compute txset hash took {:.6f}s: setup={:.6f}s, submit={:.6f}s, "
         "insert={:.6f}s, generate={:.6f}s, root={:.6f}s, txs={}, chunks={}, "
         "threads={}",
         total_elapsed, setup_elapsed, submit_elapsed, insert_elapsed,
         generate_elapsed, root_elapsed, hashes.size(), chunks.size(),
         num_threads);
  }
  return root_hash;
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
    ret_chunk[i] = tx_succeeded(result.result());
    if (ret_chunk[i]) {
      transaction_nominations.push_back(result.proposal_data());
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

MempoolCoordinatorRpc::InitializeBlockNodes
MempoolCoordinatorRpc::add_initialize_block_nodes(x23::Dag &dag,
                                                  const PendingBlock &cache,
                                                  const x23::DagDeps &deps)
{
  InitializeBlockNodes nodes;
  nodes.create_snapshot = dag.async<DbSchedResult>(
      "create proposal snapshot", deps, [&]() -> x23::Async<DbSchedResult> {
        co_return co_await create_snapshot(
            /*prev_block_id=*/last_committed_block_.id, cache.block_id,
            BlockExecutionMode::kBlockExecutionModeProposal);
      });

  // Executor proposal blocks depend on the event block being open because they
  // can send event RPCs as soon as they start processing.
  nodes.open_event = dag.async<RPCError>(
      "open event block", deps, [&]() -> x23::Async<RPCError> {
        co_return co_await eventsched_open_block(cache.block_id);
      });

  nodes.open_proposal_blocks.reserve(exec_clients_.size());
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    nodes.open_proposal_blocks.push_back(dag.async<ExecResult>(
        "open executor proposal block", dag.after(nodes.open_event),
        [&, i]() -> x23::Async<ExecResult> {
          co_return co_await open_proposal_block(cache.block_id,
                                                 /*executor_idx=*/i);
        }));
  }

  x23::DagDeps check_initialized_deps =
      dag.after(nodes.create_snapshot, nodes.open_event);
  for (const x23::DagNode<ExecResult> &node : nodes.open_proposal_blocks) {
    check_initialized_deps.ids.push_back(node.id());
  }

  x23::Vec<x23::DagNode<ExecResult>> open_proposal_blocks =
      nodes.open_proposal_blocks;
  nodes.initialized = dag.async<bool>(
      "check initialized block", std::move(check_initialized_deps),
      [&, create_snapshot = nodes.create_snapshot,
       open_event = nodes.open_event,
       open_proposal_blocks]() -> x23::Async<bool> {
        const DbSchedResult create_snapshot_result = create_snapshot.get();
        bool open_proposal_block_success = true;
        for (const x23::DagNode<ExecResult> &node : open_proposal_blocks) {
          if (node.get() != ExecResult::ExecResultOK) {
            open_proposal_block_success = false;
          }
        }

        if (create_snapshot_result != DbSchedResult::DbSchedResultOK ||
            open_event.get() != RPCError::kRPCNone ||
            !open_proposal_block_success) {
          // One of the RPCs failed, so we need to undo any of the successful
          // RPC calls.
          if (create_snapshot_result == DbSchedResult::DbSchedResultOK) {
            CHECK_EQ(co_await abandon_snapshot(cache.block_id),
                     DbSchedResult::DbSchedResultOK);
          }
          if (open_event.get() == RPCError::kRPCNone) {
            CHECK_EQ(co_await eventsched_abandon_block(cache.block_id),
                     RPCError::kRPCNone);
          }
          co_await finish_all_executors(cache.block_id);

          co_return false;
        }
        co_return true;
      });
  return nodes;
}

MempoolCoordinatorRpc::ScheduleNodes
MempoolCoordinatorRpc::add_schedule_nodes(x23::Dag &dag, PendingBlock &cache,
                                          x23::DagDeps scheduling_deps,
                                          const x23::DagDeps &proposal_deps)
{
  ScheduleNodes nodes;
  nodes.txset_trie =
      std::make_shared<MerkleTriePrefix>(Allocator<Node>::Type::kBump);
  cache.transaction_nomination_chunks.clear();
  cache.transaction_nomination_chunks.resize(kNumSchedulerIterations, {});

  struct ScheduledChunk {
    size_t start{0};
    size_t end{0};
    bool active{false};
  };

  struct SummarizedChunk {
    size_t start{0};
    size_t end{0};
    bool active{false};
  };

  struct ScheduleState {
    x23::CoroGuard schedule_guard;
    x23::Vec<size_t> handle_end_indices;
    x23::Vec<ScheduledChunk> scheduled_chunks;
    x23::Vec<SummarizedChunk> summarized_chunks;
    x23::Vec<x23::XdrVec<SignedTransactionHash>> txset_hash_chunks;
    ElapsedTimer summarize_timer;
    size_t num_tx_outcomes_processed{0};
    size_t tx_write_idx{0};
    double clear_failed_tx_hashes_seconds{0.0};
    double append_nomination_chunks_seconds{0.0};
    double compact_txs_seconds{0.0};
    double resize_txs_seconds{0.0};
    double finalize_seconds{0.0};
    size_t num_scheduled{0};
    bool stopped{false};
  };
  auto state = std::make_shared<ScheduleState>();
  state->handle_end_indices.reserve(kNumSchedulerIterations);
  state->scheduled_chunks.resize(kNumSchedulerIterations, {});
  state->summarized_chunks.resize(kNumSchedulerIterations, {});
  state->txset_hash_chunks.resize(kNumSchedulerIterations, {});
  nodes.schedule_chunks.reserve(kNumSchedulerIterations);
  nodes.propose_chunks.reserve(kNumSchedulerIterations);
  nodes.txset_insert_chunks.reserve(kNumSchedulerIterations);
  nodes.serialize_chunks.reserve(kNumSchedulerIterations);

  nodes.lock_scheduler =
      dag.async<void>("lock scheduler nomination", std::move(scheduling_deps),
                      [&, state]() -> x23::Async<void> {
                        co_await state->schedule_guard(
                            scheduler_->global_lock_);
                        scheduler_->start_pending_nomination(cache.block_id);
                        co_return;
                      });

  x23::DagNode<void> previous_schedule_node;
  for (size_t i = 0; i < kNumSchedulerIterations; i++) {
    x23::DagDeps deps = i == 0 ? dag.after(nodes.lock_scheduler)
                               : dag.after(previous_schedule_node);
    auto schedule_node =
        dag.async<void>("schedule nomination chunk", std::move(deps),
                        [&, state, i]() -> x23::Async<void> {
                          if (state->stopped) {
                            co_return;
                          }
                          scheduler_->schedule(kMaxBlockSize /
                                                   kNumSchedulerIterations,
                                               cache.tx_hashes, cache.txs);
                          const size_t start = state->num_scheduled;
                          const size_t end = cache.tx_hashes.size();
                          if (end == start) {
                            state->stopped = true;
                            co_return;
                          }
                          state->scheduled_chunks[i] = ScheduledChunk{
                              .start = start,
                              .end = end,
                              .active = true,
                          };
                          state->handle_end_indices.push_back(end);
                          state->num_scheduled = end;
                        });
    nodes.schedule_chunks.push_back(schedule_node);
    auto propose_node = dag.async<void>(
        "propose nomination chunk",
        merge_dag_deps(proposal_deps, dag.after(schedule_node)),
        [&, state, i]() -> x23::Async<void> {
          if (!state->scheduled_chunks[i].active) {
            co_return;
          }
          const size_t start = state->scheduled_chunks[i].start;
          const size_t end = state->scheduled_chunks[i].end;
          x23::span<bool> ret_span(cache.statuses.begin() + start,
                                   cache.statuses.begin() + end);
          // [schedule()] appends transactions to the end of [txs]. We reserve
          // [txs] up front in [schedulercoord_nominate()], so spans into it do
          // not become invalid when later schedule nodes append more txs.
          x23::span<SignedTransaction> txs_span(cache.txs.begin() + start,
                                                cache.txs.begin() + end);
          co_await propose_batch_in_chunks(
              cache.block_id, txs_span, cache.transaction_nomination_chunks[i],
              ret_span);
        });
    nodes.propose_chunks.push_back(propose_node);
    previous_schedule_node = schedule_node;
  }

  x23::Vec<x23::DagNode<void>> summary_nodes;
  summary_nodes.reserve(kNumSchedulerIterations);
  x23::DagNode<void> previous_summary_node;
  for (size_t i = 0; i < kNumSchedulerIterations; i++) {
    x23::DagDeps summary_deps =
        i == 0 ? dag.after(nodes.propose_chunks[i])
               : dag.after(nodes.propose_chunks[i], previous_summary_node);
    auto summary_node = dag.async<void>(
        "summarize nomination schedule", std::move(summary_deps),
        [&, state, i]() -> x23::Async<void> {
          if (i == 0) {
            state->summarize_timer = ElapsedTimer();
            ElapsedTimer part_timer;
            cache.failed_tx_hashes.clear();
            state->clear_failed_tx_hashes_seconds = part_timer.elapsed();
            CHECK(cache.result_tx_nominations.empty());
          }

          if (state->scheduled_chunks[i].active) {
            ElapsedTimer part_timer;
            const size_t result_start = cache.result_tx_hashes.size();
            x23::XdrVec<NondeterministicProposalData> &chunk_nominations =
                cache.transaction_nomination_chunks[i];
            cache.result_tx_nominations.append(
                std::make_move_iterator(chunk_nominations.begin()),
                std::make_move_iterator(chunk_nominations.end()));
            state->append_nomination_chunks_seconds += part_timer.elapsed();

            // We need to remove failed txs from [cache.txs],
            // so [tx_write_idx] tracks where to overwrite
            // successful txs to in [cache.txs].
            part_timer = ElapsedTimer();
            for (size_t tx_idx = state->num_tx_outcomes_processed;
                 tx_idx < state->handle_end_indices[i]; tx_idx++) {
              if (cache.statuses[tx_idx]) {
                const SignedTransactionHash &tx_hash = cache.tx_hashes[tx_idx];
                cache.result_tx_hashes.push_back(tx_hash);
                state->txset_hash_chunks[i].push_back(tx_hash);
                if (state->tx_write_idx != tx_idx) {
                  cache.txs[state->tx_write_idx] = std::move(cache.txs[tx_idx]);
                }
                state->tx_write_idx++;
              }
              else {
                cache.failed_tx_hashes.push_back(cache.tx_hashes[tx_idx]);
              }
            }
            state->compact_txs_seconds += part_timer.elapsed();
            state->num_tx_outcomes_processed = state->handle_end_indices[i];
            state->summarized_chunks[i] = SummarizedChunk{
                .start = result_start,
                .end = cache.result_tx_hashes.size(),
                .active = true,
            };
          }

          if (i + 1 == kNumSchedulerIterations) {
            ElapsedTimer part_timer;
            cache.txs.resize(state->tx_write_idx);
            state->resize_txs_seconds = part_timer.elapsed();

            part_timer = ElapsedTimer();
            CHECK_EQ(state->num_tx_outcomes_processed, cache.tx_hashes.size());
            CHECK_EQ(cache.result_tx_hashes.size(),
                     cache.result_tx_nominations.size());

            state->schedule_guard.reset();
            state->finalize_seconds = part_timer.elapsed();

            info("summarize nomination schedule took {:.6f}s: "
                 "clear failed tx hashes {:.6f}s, append "
                 "nomination chunks {:.6f}s, compact txs "
                 "{:.6f}s, resize txs {:.6f}s, finalize {:.6f}s",
                 state->summarize_timer.elapsed(),
                 state->clear_failed_tx_hashes_seconds,
                 state->append_nomination_chunks_seconds,
                 state->compact_txs_seconds, state->resize_txs_seconds,
                 state->finalize_seconds);
          }
          co_return;
        });
    summary_nodes.push_back(summary_node);
    previous_summary_node = summary_node;
  }
  nodes.summarize = previous_summary_node;

  for (size_t i = 0; i < kNumSchedulerIterations; i++) {
    auto txset_insert_node = dag.cpu<void>(
        "insert txset hash batch", dag.after(summary_nodes[i]),
        [&, state, i, txset_trie = nodes.txset_trie]() {
          if (!state->scheduled_chunks[i].active) {
            return;
          }
          const x23::XdrVec<SignedTransactionHash> &tx_hashes =
              state->txset_hash_chunks[i];
          WorkerPool &worker_pool = WorkerPool::get_default_pool();
          const size_t num_threads = std::max(available_parallelism() / 2, 1UL);
          const std::vector<ChunkRange> chunks =
              make_chunks(tx_hashes.size(), num_threads);
          std::vector<std::future<void>> futures;
          futures.reserve(chunks.size());
          for (const ChunkRange &chunk : chunks) {
            futures.push_back(
                worker_pool.submit([chunk, &tx_hashes, txset_trie] {
                  for (size_t j = chunk.start; j < chunk.end; j++) {
                    CHECK(txset_trie->insert(HexArray(tx_hashes[j]),
                                             std::span<const std::byte>()));
                  }
                }));
          }
          for (std::future<void> &future : futures) {
            future.get();
          }
          return;
        });
    nodes.txset_insert_chunks.push_back(txset_insert_node);
  }

  x23::DagNode<void> previous_serialize_node;
  for (size_t i = 0; i < kNumSchedulerIterations; i++) {
    x23::DagDeps serialize_deps =
        i == 0 ? dag.after(summary_nodes[i])
               : dag.after(summary_nodes[i], previous_serialize_node);
    auto serialize_node = dag.async<void>(
        "serialize nomination batch", std::move(serialize_deps),
        [&, state, i]() -> x23::Async<void> {
          if (i == 0) {
            Nomination &nomination =
                cache.nomination_result.nominated_block().nomination;
            nomination.transactions_hash = {};
            nomination.transactions_nondeterministic_data = {};
            cache.nomination_result.nominated_block().txs = {};
          }
          if (!state->summarized_chunks[i].active) {
            co_return;
          }
          const size_t start = state->summarized_chunks[i].start;
          const size_t end = state->summarized_chunks[i].end;
          x23::span<const SignedTransactionHash> tx_hashes_span(
              cache.result_tx_hashes.begin() + start,
              cache.result_tx_hashes.begin() + end);
          x23::span<const NondeterministicProposalData> nominations_span(
              cache.result_tx_nominations.begin() + start,
              cache.result_tx_nominations.begin() + end);
          x23::span<const SignedTransaction> txs_span(cache.txs.begin() + start,
                                                      cache.txs.begin() + end);
          co_await default_coropoll().pool_.bg();
          Nomination &nomination =
              cache.nomination_result.nominated_block().nomination;
          append_serialized_segment(tx_hashes_span,
                                    nomination.transactions_hash);
          append_serialized_segment(
              nominations_span, nomination.transactions_nondeterministic_data);
          append_serialized_segment(
              txs_span, cache.nomination_result.nominated_block().txs);
          co_return;
        });
    nodes.serialize_chunks.push_back(serialize_node);
    previous_serialize_node = serialize_node;
  }
  return nodes;
}

MempoolCoordinatorRpc::FinishNominationNodes
MempoolCoordinatorRpc::add_finish_nominating_block_nodes(
    x23::Dag &dag, const DbTip &db_tip, PendingBlock &cache,
    const x23::DagDeps &proposal_deps,
    std::shared_ptr<MerkleTriePrefix> txset_trie,
    const x23::DagDeps &txset_hash_deps, const x23::DagDeps &serialized_deps)
{
  FinishNominationNodes nodes;
  nodes.finish_executors =
      dag.async<void>("finish executors", proposal_deps,
                      [&]() -> x23::Async<void> {
                        co_await finish_all_executors(cache.block_id);
                        co_return;
                      });
  nodes.close_snapshot =
      dag.async<DbBlockResult>("close proposal snapshot", proposal_deps,
                               [&]() -> x23::Async<DbBlockResult> {
                                 co_return co_await close_snapshot(
                                     cache.block_id);
                               });
  nodes.close_event =
      dag.async<EventBlockResult>("close event block", proposal_deps,
                                  [&]() -> x23::Async<EventBlockResult> {
                                    co_return co_await eventsched_close_block(
                                        cache.block_id, db_tip.height + 1);
                                  });
  nodes.txset_hash =
      dag.cpu<SHA256Hash>("compute txset hash", txset_hash_deps, [txset_trie] {
        ElapsedTimer timer;
        const size_t num_threads = available_parallelism();
        txset_trie->generate_all_hashes(num_threads);
        const double generate_elapsed = timer.elapsed();
        SHA256Hash root_hash = txset_trie->get_root_hash();
        const double root_elapsed = timer.elapsed() - generate_elapsed;
        info("compute txset hash took {:.6f}s: generate={:.6f}s, root={:.6f}s, "
             "threads={}",
             timer.elapsed(), generate_elapsed, root_elapsed, num_threads);
        return root_hash;
      });
  nodes.serialize_nomination =
      dag.async<void>("serialize nomination batches", serialized_deps,
                      [&]() -> x23::Async<void> { co_return; });
  nodes.build_header = dag.async<void>(
      "build nomination header",
      dag.after(nodes.close_snapshot, nodes.close_event, nodes.txset_hash),
      [&, close_snapshot = nodes.close_snapshot,
       close_event = nodes.close_event,
       txset_hash = nodes.txset_hash]() -> x23::Async<void> {
        {
          DbBlockResult result = close_snapshot.get();
          CHECK_EQ(result.result(), DbSchedResult::DbSchedResultOK);
          cache.db_tip.db_hash = result.db_hash();
        }
        {
          EventBlockResult result = close_event.get();
          CHECK_EQ(result.error(), RPCError::kRPCNone);
          cache.db_tip.events_hash = result.root_hash();
        }
        Nomination &nomination =
            cache.nomination_result.nominated_block().nomination;
        nomination.block_height = db_tip.height + 1;

        BlockHeader header{.network_id = network_id_,
                           .block_height = nomination.block_height,
                           .txset_hash = txset_hash.get(),
                           .db_hash = cache.db_tip.db_hash,
                           .events_hash = cache.db_tip.events_hash,
                           .prev_block_hash = db_tip.block_hash};
        nomination.block_hash = sha256(header);
        nomination.prev_block_hash = db_tip.block_hash;
        co_return;
      });
  return nodes;
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
  x23::Async<DbBlockResult> commit_snapshot_handle =
      commit_snapshot(cache.block_id, cache.db_tip.block_hash,
                      cache.db_tip.events_hash);
  x23::Async<RPCError> event_commit_handle =
      eventsched_commit_block(cache.block_id);
  {
    DbBlockResult result = co_await commit_snapshot_handle;
    CHECK_EQ(result.result(), DbSchedResult::DbSchedResultOK);
  }
  CHECK_EQ(co_await event_commit_handle, RPCError::kRPCNone);
}

x23::Async<SchedulerNominationResult>
MempoolCoordinatorRpc::schedulercoord_nominate(const DbTip db_tip)
{
  CHECK(ready());

  ElapsedTimer timer;
  NominationTelemetry nomination_telemetry;
  x23::Defer record_nomination_stats([&] {
    nomination_telemetry.duration = timer.elapsed();
    record_metric_cleanup(
        [&] { record_nomination_attempt(nomination_telemetry); });
  });
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
    info("schedulercoord_nominate ret");
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }
  if (nominated_block_cache_.block_id) {
    // A block was nominated by this node and not yet committed or abandoned.
    // [schedulercoord_commit_block()] needs to be called before this node can
    // nominate another block.
    info("schedulercoord_nominate ret");
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  x23::Dag nomination_dag("schedulercoord_nominate", default_threadpool());

  // HOG transfers and auth changes from the previous block both affect the
  // scheduling priorities for this block, but neither depends on the other.
  auto transfer_data_node = nomination_dag.async<void>(
      "wait transfer data", [&]() -> x23::Async<void> {
        co_await wait_for_transfer_data(db_tip.height);
        co_return;
      });
  auto auth_data_node =
      nomination_dag.async<void>("wait auth data", [&]() -> x23::Async<void> {
        co_await wait_for_auth_data(db_tip.height);
        co_return;
      });

  auto prepare_cache_node = nomination_dag.async<void>(
      "prepare nomination cache", [&]() -> x23::Async<void> {
        nominated_block_cache_.block_id = generate_block_id();
        // We place the block txs and tx hashes into these vectors. We mark
        // these vectors static and clear them on each call to [nominate()] so
        // to avoid memory allocation/free overhead.
        x23::XdrVec<SignedTransaction> tmp =
            std::move(nominated_block_cache_.txs);
        [](x23::XdrVec<SignedTransaction> to_clear) -> x23::Detached {
          co_await default_coropoll().pool_.bg();
          to_clear.clear();
        }(std::move(tmp));

        nominated_block_cache_.txs = {};
        nominated_block_cache_.txs.reserve(kMaxBlockSize);
        nominated_block_cache_.tx_hashes.clear();
        nominated_block_cache_.result_tx_hashes.clear();
        nominated_block_cache_.result_tx_nominations.clear();
        nominated_block_cache_.txs.reserve(kMaxBlockSize);
        nominated_block_cache_.tx_hashes.reserve(kMaxBlockSize);
        nominated_block_cache_.result_tx_hashes.reserve(kMaxBlockSize);
        nominated_block_cache_.result_tx_nominations.reserve(kMaxBlockSize);
        // We set the outcome here because doing so resets
        // [nominated_block_cache_.nomination_result.nominated_block()]. We
        // fill in [nominated_block()] throughout this function, so we want our
        // writes to it preserved.
        nominated_block_cache_.nomination_result.outcome(
            SchedulerNominationOutcome::SchedulerNominationOK);
        co_return;
      });

  InitializeBlockNodes initialize_block_nodes =
      add_initialize_block_nodes(nomination_dag, nominated_block_cache_,
                                 nomination_dag.after(transfer_data_node,
                                                      auth_data_node,
                                                      prepare_cache_node));
  auto check_initialize_block_node = nomination_dag.async<void>(
      "check initialize block",
      nomination_dag.after(initialize_block_nodes.initialized),
      [&]() -> x23::Async<void> {
        if (!initialize_block_nodes.initialized.get()) {
          nomination_dag.fail(x23::DagFailureKind::Cancelled,
                              "initialize block");
        }
        co_return;
      });
  ScheduleNodes schedule_nodes =
      add_schedule_nodes(nomination_dag, nominated_block_cache_,
                         nomination_dag.after(transfer_data_node,
                                              auth_data_node,
                                              prepare_cache_node),
                         nomination_dag.after(check_initialize_block_node));
  add_finish_nominating_block_nodes(
      nomination_dag, db_tip, nominated_block_cache_,
      dag_deps_from_nodes(schedule_nodes.propose_chunks),
      schedule_nodes.txset_trie,
      dag_deps_from_nodes(schedule_nodes.txset_insert_chunks),
      dag_deps_from_nodes(schedule_nodes.serialize_chunks));

  co_await nomination_dag.run();
  if (!nomination_dag.ok() && nomination_dag.failure().exception) {
    std::rethrow_exception(nomination_dag.failure().exception);
  }
  if (const char *home = std::getenv("HOME")) {
    std::ofstream graph_dot(std::string(home) + "/graph.dot",
                            std::ios::out | std::ios::trunc);
    graph_dot << nomination_dag.dot();
  }

  const bool initialized_block = initialize_block_nodes.initialized.get();
  if (!initialized_block) {
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
    info("schedulercoord_nominate ret");
    co_return SchedulerNominationResult(
        SchedulerNominationOutcome::SchedulerNominationError);
  }

  // We set [nominated_block_cache_.nomination_result.outcome()] to
  // [SchedulerNominationOutcome::SchedulerNominationOK] above. See the comments
  // there.
  nominated_block_cache_.nomination_result.nominated_block().block_id =
      nominated_block_cache_.block_id;
  nomination_telemetry.succeeded = true;
  nomination_telemetry.scheduled_tx_count =
      nominated_block_cache_.tx_hashes.size();
  nomination_telemetry.nominated_tx_count =
      nominated_block_cache_.result_tx_hashes.size();
  nomination_telemetry.failed_tx_count =
      nominated_block_cache_.failed_tx_hashes.size();
  nominated_block_cache_.db_tip = DbTip{
      .height = nominated_block_cache_.nomination_result.nominated_block()
                    .nomination.block_height,
      .block_hash = nominated_block_cache_.nomination_result.nominated_block()
                        .nomination.block_hash,
      .events_hash = nominated_block_cache_.db_tip.events_hash,
      .db_hash = nominated_block_cache_.db_tip.db_hash,
      .id =
          nominated_block_cache_.nomination_result.nominated_block().block_id};
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
  // Validation succeeds if every entry in the executor response is a
  // deterministic outcome (OK, deterministic error, or root frame revert).
  // System errors are nondeterministic and cause validation to fail.
  co_return !(std::ranges::any_of(res, tx_failed));
}

x23::Async<MempoolCoordinatorRpc::PreparedValidationChunk>
MempoolCoordinatorRpc::prepare_validation_chunk(
    x23::span<const SignedTransactionHash> tx_hashes,
    x23::span<const NondeterministicProposalData> tx_nominations)
{
  PreparedValidationChunk prepared;
  prepared.proposed_txs.resize(tx_hashes.size());
  x23::Vec<bool> tx_found(tx_hashes.size(), false);

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
           std::shared_ptr<Scheduler> scheduler) noexcept -> x23::Async<void> {
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
                                                          tx_found,
                                                          scheduler_));
  }
  for (x23::Async<void> &handle : lookup_handles) {
    co_await handle;
  }

  for (size_t i = 0; i < tx_found.size(); i++) {
    if (!tx_found[i]) {
      prepared.missing_tx_hashes.push_back(tx_hashes[i]);
    }
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

  PreparedValidationChunk prepared =
      co_await prepare_validation_chunk(tx_hashes, tx_nominations);

  PreparedAndValidatedChunk ret{
      .missing_tx_hashes = std::move(prepared.missing_tx_hashes),
      .validated = true,
  };
  if (!ret.missing_tx_hashes.empty()) {
    co_return ret;
  }

  ret.validated = co_await validate_batch(block_id, prepared.proposed_txs);
  ret.txs.reserve(prepared.proposed_txs.size());
  for (ProposedTransaction &proposed_tx : prepared.proposed_txs) {
    ret.txs.push_back(std::move(proposed_tx.signed_tx));
  }
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
  CHECK(ready());

  ElapsedTimer timer;
  SchedulerValidationOutcome validation_outcome =
      SchedulerValidationOutcome::SchedulerValidationInvalid;
  size_t validation_tx_count = 0;
  x23::Defer record_validation_stats([&] {
    record_metric_cleanup([&] {
      record_validation_attempt(validation_outcome, timer.elapsed(),
                                validation_tx_count);
    });
  });
  while (true) {
    while (!(co_await increment_num_validations_in_progress())) {
      co_await wait_for_validation_gate_open();
    }

    if (nomination.block_height < last_committed_block_.height + 1) {
      // This block is too old to validate.
      co_await decrement_num_validations_in_progress();
      validation_outcome =
          SchedulerValidationOutcome::SchedulerValidationTooLateToValidate;
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
    validation_outcome = SchedulerValidationOutcome::SchedulerValidationInvalid;
    info("schedulercoord_validate ret");
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

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

    x23::Async<bool> tx_nondeterministic_data_handle =
        parallel_batch_to_t(nomination.transactions_nondeterministic_data,
                            tx_nominations);
    x23::Async<bool> tx_hash_handle =
        parallel_batch_to_t(nomination.transactions_hash, cache.tx_hashes);

    const bool decoded_tx_nominations =
        co_await tx_nondeterministic_data_handle;

    const bool decoded_tx_hashes = co_await tx_hash_handle;

    if (!decoded_tx_nominations || !decoded_tx_hashes) {
      co_await decrement_num_validations_in_progress();
      validation_outcome =
          SchedulerValidationOutcome::SchedulerValidationMalformed;
      info("schedulercoord_validate rejected malformed nomination batch");
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationMalformed);
    }

    if (cache.tx_hashes.size() != tx_nominations.size()) {
      co_await decrement_num_validations_in_progress();
      validation_outcome =
          SchedulerValidationOutcome::SchedulerValidationMalformed;
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationMalformed);
    }

    // TODO(jhumphri): Remember that we need to pre-size the vectors in [cache]
    // correctly when we use the helper functions above to send txs to the
    // executors.
    cache.block_id = generate_block_id();
    validation_tx_count = cache.tx_hashes.size();
  }

  co_await wait_for_transfer_data(nomination.block_height - 1);
  co_await wait_for_auth_data(nomination.block_height - 1);

  DbSchedResult snapshot_result = co_await create_snapshot(
      /*prev_block_id=*/db_tip.id, cache.block_id,
      BlockExecutionMode::kBlockExecutionModeValidation);
  if (snapshot_result != DbSchedResult::DbSchedResultOK) {
    co_await decrement_num_validations_in_progress();
    if (snapshot_result == DbSchedResult::DbSchedResultInvalidParentId) {
      validation_outcome =
          SchedulerValidationOutcome::SchedulerValidationUnknownParentBlock;
      info("schedulercoord_validate ret");
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationUnknownParentBlock);
    }
    info("schedulercoord_validate ret");
    CHECK_EQ(snapshot_result, DbSchedResult::DbSchedResultError);
    validation_outcome =
        SchedulerValidationOutcome::SchedulerValidationDbFailed;
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
    co_await finish_all_executors(cache.block_id);
    co_await abandon_block(cache);
    co_await decrement_num_validations_in_progress();
    validation_outcome =
        SchedulerValidationOutcome::SchedulerValidationExecutorFailed;
    info("schedulercoord_validate ret");
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationExecutorFailed);
  }

  // [nomination] includes the tx hashes and the nondeterministic proposal
  // data. We need to use these tx hashes to look up the vetted txs in
  // [vetted_txs_]. Assuming we find all of the txs, we then combine them with
  // the nondeterministic proposal data to form a [ProposedTransaction], which
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
      validated = chunk_result.validated && validated;
      missing_tx_hashes += chunk_result.missing_tx_hashes;
      cache.txs += chunk_result.txs;
    }

    for (const SignedTransactionHash &tx_hash : missing_tx_hashes) {
      if (tx_failed_authentication(tx_hash)) {
        co_await finish_all_executors(cache.block_id);
        co_await abandon_block(cache);
        co_await decrement_num_validations_in_progress();
        validation_outcome =
            SchedulerValidationOutcome::SchedulerValidationInvalid;
        info("schedulercoord_validate ret");
        co_return SchedulerValidationResult(
            SchedulerValidationOutcome::SchedulerValidationInvalid);
      }
    }

    if (!missing_tx_hashes.empty()) {
      co_await finish_all_executors(cache.block_id);
      co_await abandon_block(cache);
      co_await decrement_num_validations_in_progress();
      validation_outcome =
          SchedulerValidationOutcome::SchedulerValidationMissingTxs;

      SchedulerValidationResult ret(
          SchedulerValidationOutcome::SchedulerValidationMissingTxs);
      ret.missing_tx_hashes() = missing_tx_hashes;
      info("schedulercoord_validate ret");
      co_return ret;
    }
    CHECK_EQ(cache.txs.size(), tx_nominations.size());
  }

  if (!validated) {
    co_await finish_all_executors(cache.block_id);
    co_await abandon_block(cache);
    co_await decrement_num_validations_in_progress();
    validation_outcome = SchedulerValidationOutcome::SchedulerValidationInvalid;
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
      // I am deliberately co_await'ing [event_close_block_handle] first since
      // [close_snapshot()] (the other RPC call above) can fail, and I would
      // rather not have to deal with co_await'ing [event_close_block_handle] as
      // well in the error path for that.
      EventBlockResult result = co_await event_close_block_handle;
      CHECK_EQ(result.error(), RPCError::kRPCNone);
      cache.db_tip.events_hash = result.root_hash();
    }
    {
      DbBlockResult result = co_await close_snapshot_handle;
      // A forged RCQ observation can pass executor validation and be rejected
      // only by hogdb at [dbsched_close_snapshot()], so an invalid nominated
      // block can reach this path.
      if (result.result() != DbSchedResult::DbSchedResultOK) {
        CHECK_EQ(co_await abandon_snapshot(cache.block_id),
                 DbSchedResult::DbSchedResultOK);
        co_await finish_block_handle;
        co_await decrement_num_validations_in_progress();
        validation_outcome =
            SchedulerValidationOutcome::SchedulerValidationInvalid;
        info("schedulercoord_validate ret");
        co_return SchedulerValidationResult(
            SchedulerValidationOutcome::SchedulerValidationInvalid);
      }
      cache.db_tip.db_hash = result.db_hash();
    }
  }

  BlockHeader header{.network_id = network_id_,
                     .block_height = nomination.block_height,
                     .txset_hash = txset_hash,
                     .db_hash = cache.db_tip.db_hash,
                     .events_hash = cache.db_tip.events_hash,
                     .prev_block_hash = nomination.prev_block_hash};
  SHA256Hash block_hash = sha256(header);
  if (block_hash != nomination.block_hash) {
    co_await finish_block_handle;
    // The event block and DB snapshot were closed above, but a hash mismatch
    // means this block is never added to [validated_block_cache_], so
    // [schedulercoord_commit_block] will never abandon it. Abandon both here to
    // avoid leaking the event block context and orphaning its events file.
    co_await abandon_block(cache);
    co_await decrement_num_validations_in_progress();
    validation_outcome = SchedulerValidationOutcome::SchedulerValidationInvalid;
    info("schedulercoord_validate ret");
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }
  co_await finish_block_handle;

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

  const BlockID block_id = cache.block_id;
  {
    x23::CoroGuard cache_cg;
    co_await cache_cg(validated_block_cache_lock_);
    CHECK(!validated_block_cache_.contains(cache.block_id));
    validated_block_cache_[block_id] = std::move(cache);
  }
  co_await decrement_num_validations_in_progress();
  validation_outcome = SchedulerValidationOutcome::SchedulerValidationOK;
  info("schedulercoord_validate ret");
  co_return result;
}

// TODO(jhumphri): We could livelock in this method if we continue to receive
// blocks to validate. We do not pause validations once
// [schedulercoord_commit_block()] starts. If this becomes an issue (such as due
// to a malicious node that keeps sending blocks to validate), we will need to
// stop validations once a commit starts.
x23::Async<SchedulerCommitOutcome>
MempoolCoordinatorRpc::schedulercoord_commit_block(BlockID block_id)
{
  CHECK(ready());
  MempoolMetrics::instance().commit_attempt_count.increment();

  const std::chrono::steady_clock::time_point commit_started_at =
      std::chrono::steady_clock::now();
  {
    std::chrono::steady_clock::duration delta =
        commit_started_at - last_commit_;
    auto delta_raw =
        std::chrono::duration_cast<std::chrono::milliseconds>(delta);
    MempoolMetrics::instance().last_commit_interval_ms.record(
        delta_raw.count());
  }
  ElapsedTimer timer;
  auto emit_commit_stats = [&](CommitMetricsOutcome outcome,
                               size_t reenqueue_size,
                               size_t remove_from_scheduler_size) {
    MempoolMetrics::instance().last_commit_duration_ms.record(
        mempool_duration_ms(timer.elapsed()));
    switch (outcome) {
    case CommitMetricsOutcome::kOk:
      MempoolMetrics::instance().commit_success_count.increment();
      break;
    case CommitMetricsOutcome::kBlockNotFound:
      MempoolMetrics::instance().commit_block_not_found_count.increment();
      break;
    }
    MempoolMetrics::instance().commit_reenqueued_tx_count.increment(
        reenqueue_size);
    MempoolMetrics::instance().commit_removed_tx_count.increment(
        remove_from_scheduler_size);
  };

  co_await close_validation_gate();
  x23::Defer reopen_validation_gate([this] { open_validation_gate(); });

  x23::CoroGuard nominate_cg;
  co_await nominate_cg(nominated_block_cache_lock_);

  if ((!nominated_block_cache_.block_id ||
       nominated_block_cache_.block_id != block_id) &&
      !validated_block_cache_.contains(block_id)) {
    emit_commit_stats(CommitMetricsOutcome::kBlockNotFound,
                      /*reenqueue_size=*/0,
                      /*remove_from_scheduler_size=*/0);
    // A block with ID [block_id] was neither nominated nor validated, so it
    // cannot be committed.
    info("schedulercoord_commit_block ret");
    co_return SchedulerCommitOutcome::SchedulerCommitBlockNotFound;
  }

  x23::CoroGuard commit_cg;
  co_await commit_cg(commit_lock_);

  // We use [scheduler_->pending_hog_age_update_lock_] to synchronize hog age
  // updates with [MempoolDbRpc], which calls [scheduler_->process_transfers()].
  // We want to ensure that on a block commit, we are able to apply the pending
  // hog ages in the scheduler *before* the hog transfer deltas are received
  // from hogdb. Otherwise, if the transfer deltas are applied first, it is
  // possible that they will transfer hogs between accounts, and then when we
  // apply the pending hog ages after, we may try to move hogs between scheduler
  // groups for an account that no longer has any hogs.
  x23::CoroGuard hog_age_cg;
  co_await hog_age_cg(scheduler_->pending_hog_age_update_lock_);

  x23::Vec<x23::Async<void>> commit_handles;
  commit_handles.reserve(validated_block_cache_.size());
  x23::Vec<x23::Async<DbSchedResult>> snapshot_handles;
  snapshot_handles.reserve(validated_block_cache_.size());
  if (nominated_block_cache_.block_id &&
      nominated_block_cache_.block_id == block_id) {
    // We are committing the block that we nominated.
    commit_handles.push_back(commit_block(nominated_block_cache_));
    last_committed_block_ = nominated_block_cache_.db_tip;
  }
  else {
    // We are committing a block that we validated.
    commit_handles.push_back(commit_block(validated_block_cache_[block_id]));
    if (nominated_block_cache_.block_id) {
      // We nominated a block, so we need to abandon the block.
      snapshot_handles.push_back(
          abandon_snapshot(nominated_block_cache_.block_id));
      commit_handles.push_back(
          [](MempoolCoordinatorRpc &self, BlockID id) -> x23::Async<void> {
            CHECK_EQ(co_await self.eventsched_abandon_block(id),
                     RPCError::kRPCNone);
          }(*this, nominated_block_cache_.block_id));
    }
    last_committed_block_ = validated_block_cache_[block_id].db_tip;
  }
  for (const auto &[_, cache] : validated_block_cache_) {
    if (cache.block_id != block_id) {
      snapshot_handles.push_back(abandon_snapshot(cache.block_id));
      commit_handles.push_back(
          [](MempoolCoordinatorRpc &self, BlockID id) -> x23::Async<void> {
            CHECK_EQ(co_await self.eventsched_abandon_block(id),
                     RPCError::kRPCNone);
          }(*this, cache.block_id));
    }
  }

  clear_expired_failed_authentication_txs();

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
  // and were added to [failed_tx_hashes].
  static std::unordered_set<SignedTransactionHash, ByteArrayHasher>
      return_to_runqueue;
  return_to_runqueue.clear();
  const x23::XdrVec<SignedTransactionHash> *remove_from_scheduler = nullptr;
  if (nominated_block_cache_.block_id &&
      nominated_block_cache_.block_id == block_id) {
    // The committed block is the block we nominated.
    // [nominated_block_cache_.tx_hashes] also includes the failed txs (in
    // [nominated_block_cache_.failed_tx_hashes]).
    remove_from_scheduler = &nominated_block_cache_.tx_hashes;
  }
  else {
    // The committed block is a block we validated.
    if (nominated_block_cache_.block_id &&
        nominated_block_cache_.block_id != block_id) {
      // Return the nominated block's txs to the runqueue so they can be
      // scheduled again. [nominated_block_cache_.tx_hashes] also includes the
      // failed txs (in [nominated_block_cache_.failed_tx_hashes]).
      return_to_runqueue.insert(nominated_block_cache_.tx_hashes.begin(),
                                nominated_block_cache_.tx_hashes.end());
    }
    for (const auto &[_, cache] : validated_block_cache_) {
      if (cache.block_id == block_id) {
        CHECK_NULL(remove_from_scheduler);
        remove_from_scheduler = &cache.tx_hashes;
      }
    }
    CHECK_PTR(remove_from_scheduler);
    // Any of the txs in the committed (validated) block that were also in the
    // nominated block should not be returned to the runqueue.
    for (const SignedTransactionHash &hash : *remove_from_scheduler) {
      if (return_to_runqueue.empty()) {
        break;
      }
      return_to_runqueue.erase(hash);
    }
  }

  scheduler_->reenqueue_txs(return_to_runqueue.cbegin(),
                            return_to_runqueue.cend());
  CHECK_PTR(remove_from_scheduler);
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
    CHECK_PTR(committed_cache);
    // [apply_committed_block_hog_ages] acquires [scheduler_->global_lock_] for
    // us, so we do not need to do it here.
    co_await scheduler_->apply_committed_block_hog_ages(
        committed_cache->txs, committed_cache->db_tip.height);
  }

  for (x23::Async<void> &handle : commit_handles) {
    co_await handle;
  }
  for (x23::Async<DbSchedResult> &handle : snapshot_handles) {
    CHECK_EQ(co_await handle, DbSchedResult::DbSchedResultOK);
  }

  // Be careful to not do [nominated_block_cache_ = {}]. We pre-allocate
  // elements in the vectors in [nominated_block_cache_], so if we clear those
  // vectors, we will experience segfaults because we will later attempt to
  // read/write indices that do not exist.
  nominated_block_cache_.block_id = 0;
  validated_block_cache_.clear();

  scheduler_->increment_current_block();
  co_await Health::instance().set_current_block_height(
      last_committed_block_.height);
  MempoolMetrics::instance().current_block_height.record(
      last_committed_block_.height);
  MempoolMetrics::instance().scheduler_pending_tx_count.record(
      scheduler_->size());
  MempoolMetrics::instance().scheduler_tx_cache_size.record(
      scheduler_->tx_cache_size());
  emit_commit_stats(CommitMetricsOutcome::kOk, return_to_runqueue.size(),
                    remove_from_scheduler_size);
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
  CHECK(ready());
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
