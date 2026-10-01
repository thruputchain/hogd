#include "hogd/hog_mempool/rpc_from_coord.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "auth_rpc.hh"
#include "consensus.hh"
#include "coord_sched.hh"
#include "db_sched.hh"
#include "event_rpc.hh"
#include "exec_result.hh"
#include "exec_sched.hh"
#include "header.hh"
#include "hogd/base/block_id.h"
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
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_mempool/config_hash.h"
#include "hogd/hog_mempool/scheduler.h"
#include "hogd/hog_mempool/shared.h"
#include "parallel.hh"
#include "proposal.hh"
#include "proposal_hash.h"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "sched_event.hh"
#include "sched_gateway.hh"
#include "transaction.hh"
#include "types.hh"
#include "validator.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/corosync.h"
#include "x23/dag.h"
#include "x23/vec.h"

namespace hogd {

namespace {

enum class CommitMetricsOutcome { kOk, kBlockNotFound, kConcurrent };

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

x23::Async<void>
hash_transactions(const SignedTransactionBatch &txs,
                  TransactionHashBatch &tx_hashes)
{
  tx_hashes.resize(txs.size());
  const std::vector<ChunkRange> chunks =
      make_chunks(txs.size(), available_parallelism());
  std::vector<x23::Async<void>> handles;
  handles.reserve(chunks.size());
  for (const ChunkRange &chunk : chunks) {
    handles.push_back(
        [](const ChunkRange *chunk, const SignedTransactionBatch &txs,
           TransactionHashBatch &tx_hashes) noexcept -> x23::Async<void> {
          co_await default_coropoll().pool_.bg();
          for (size_t i = chunk->start; i < chunk->end; i++) {
            tx_hashes[i] = sha256(txs[i].tx);
          }
          co_return;
        }(&chunk, txs, tx_hashes));
  }
  for (x23::Async<void> &handle : handles) {
    co_await handle;
  }
  co_return;
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

SchedulerTransactionStatus
transaction_status_for_exec(const ExecResult result)
{
  switch (result) {
  case ExecResult::ExecResultMalformedTransaction:
  case ExecResult::ExecResultTxValidationFailure:
  case ExecResult::ExecResultFailedGasSponsorship:
    return SchedulerTransactionStatus::SchedulerTransactionDropped;
  default:
    return SchedulerTransactionStatus::SchedulerTransactionRetry;
  }
}

SchedulerTransactionStatus
transaction_status_for_auth(const AuthErrorCode result)
{
  switch (result) {
  case AuthErrorCode::kAuthSuccess:
    return SchedulerTransactionStatus::SchedulerTransactionSucceeded;
  case AuthErrorCode::kAuthFailed:
    return SchedulerTransactionStatus::SchedulerTransactionDropped;
  case AuthErrorCode::kAuthStaleBlockHeight:
  case AuthErrorCode::kAuthInternal:
    return SchedulerTransactionStatus::SchedulerTransactionRetry;
  default:
    CHECK(false);
    return SchedulerTransactionStatus::SchedulerTransactionDropped;
  }
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

SchedGatewaySubmitOutcome
submit_outcome_for_auth(AuthErrorCode code)
{
  switch (code) {
  case AuthErrorCode::kAuthSuccess:
    return SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted;
  case AuthErrorCode::kAuthFailed:
    return SchedGatewaySubmitOutcome::SchedGatewaySubmitAuthenticationRejected;
  case AuthErrorCode::kAuthStaleBlockHeight:
    return SchedGatewaySubmitOutcome::SchedGatewaySubmitRetryable;
  case AuthErrorCode::kAuthInternal:
    return SchedGatewaySubmitOutcome::SchedGatewaySubmitInternalError;
  }
  error("Invalid auth error code: {}", static_cast<int>(code));
  return SchedGatewaySubmitOutcome::SchedGatewaySubmitInternalError;
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
  if (!ready()) {
    co_return;
  }
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
  co_await submit_unvetted(std::move(batch),
                           /*malformed_is_protocol_error=*/false);
  co_return;
}

x23::Async<SchedGatewaySubmitOutcomeBatch>
MempoolCoordinatorRpc::submit_unvetted_with_outcomes(
    ParallelSignedTransactionBatch batch)
{
  ElapsedTimer timer;
  x23::Defer record_duration([&] {
    record_metric_cleanup([&] {
      MempoolMetrics::instance().last_submit_unvetted_duration_ms.record(
          mempool_duration_ms(timer.elapsed()));
    });
  });
  co_return co_await submit_unvetted(std::move(batch),
                                     /*malformed_is_protocol_error=*/true);
}

x23::Async<AuthErrorCodeBatch>
MempoolCoordinatorRpc::verify_submission(const SignedTransactionBatch &txs,
                                         x23::CoroGuard &commit_guard)
{
  auto pending = std::make_shared<SubmissionAuth>();
  for (size_t i = 0; i < txs.size(); i++) {
    const ContractAddress &account = txs[i].tx.invocation.invoked_address;
    pending->accounts[account].push_back(i);
    pending->dirty.insert(account);
  }

  AuthErrorCodeBatch results;
  results.resize(txs.size());

  co_await commit_guard(commit_lock_);

  std::erase_if(submission_auth_,
                [](const std::weak_ptr<SubmissionAuth> &entry) {
                  return entry.expired();
                });
  submission_auth_.push_back(pending);
  BlockHeight verify_height = scheduler_->current_block();
  while (true) {
    const BlockHeight height = scheduler_->current_block();
    // Wait for the [auth_data_changed()] callback to complete for [height].
    if (auth_data_height_.load() < height) {
      commit_guard.reset();
      co_await wait_for_block_commit(height);
      co_await commit_guard(commit_lock_);
      continue;
    }

    // The authentication server cannot authenticate txs against an older block
    // height--it returns [AuthErrorCode::kAuthStaleBlockHeight] in this case.
    if (verify_height != height) {
      for (size_t i = 0; i < results.size(); i++) {
        if (results[i] == AuthErrorCode::kAuthStaleBlockHeight) {
          pending->dirty.insert(txs[i].tx.invocation.invoked_address);
        }
      }
    }
    if (pending->dirty.empty()) {
      co_return results;
    }

    std::unordered_set<ContractAddress, SipHash24Hasher> dirty =
        std::exchange(pending->dirty, {});
    verify_height = height;
    commit_guard.reset();

    SignedTransactionBatch retry_txs;
    x23::Vec<size_t> indices;
    for (const ContractAddress &account : dirty) {
      for (size_t i : pending->accounts.at(account)) {
        retry_txs.push_back(txs[i]);
        indices.push_back(i);
      }
    }
    if (submission_auth_test_hook_) {
      co_await submission_auth_test_hook_(verify_height, retry_txs,
                                          /*after_auth=*/false);
    }
    const AuthErrorCodeBatch verified =
        co_await auth_client_->authmempool_verify_transactions(verify_height,
                                                               retry_txs);
    if (submission_auth_test_hook_) {
      co_await submission_auth_test_hook_(verify_height, retry_txs,
                                          /*after_auth=*/true);
    }
    CHECK_EQ(verified.size(), indices.size());
    for (size_t i = 0; i < indices.size(); i++) {
      results[indices[i]] = verified[i];
    }
    // Changes during this RPC remain in [pending->dirty] for the next pass.
    co_await commit_guard(commit_lock_);
  }
}

x23::Async<SchedGatewaySubmitOutcomeBatch>
MempoolCoordinatorRpc::submit_unvetted(ParallelSignedTransactionBatch batch,
                                       bool malformed_is_protocol_error)
{
  auto txs = std::make_shared<SignedTransactionBatch>();
  auto original_indices = std::make_shared<x23::Vec<size_t>>();
  auto outcomes = std::make_shared<SchedGatewaySubmitOutcomeBatch>();
  auto tx_hashes = std::make_shared<TransactionHashBatch>();
  auto commit_guard = std::make_shared<x23::CoroGuard>();
  x23::Dag submit_dag("schedulercoord_submit_unvetted", default_threadpool());

  auto decode_node =
      submit_dag.async<void>("decode batch", [&]() -> x23::Async<void> {
        // Not [mempool_tx_size_limit_]: [enforce capacity] below admits an
        // over-capacity batch up to the free slots rather than dropping it.
        if (!(co_await parallel_batch_to_t(batch, *txs, kNoBatchSizeLimit))) {
          MempoolMetrics::instance()
              .rejected_malformed_unvetted_rpc_count.increment();
          info("submit_unvetted rejected malformed transaction batch");
          if (malformed_is_protocol_error) {
            throw std::runtime_error("malformed unvetted transaction batch");
          }
          submit_dag.fail(x23::DagFailureKind::Cancelled, "decode batch");
          co_return;
        }
        record_submitted_unvetted(txs->size());
        outcomes->resize(txs->size());
        std::fill(outcomes->begin(), outcomes->end(),
                  SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
        original_indices->reserve(txs->size());
        for (size_t i = 0; i < txs->size(); i++) {
          original_indices->push_back(i);
        }
        co_return;
      });

  auto ready_node = submit_dag.async<void>(
      "check readiness", submit_dag.after(decode_node),
      [&]() -> x23::Async<void> {
        if (!ready()) {
          std::fill(outcomes->begin(), outcomes->end(),
                    SchedGatewaySubmitOutcome::SchedGatewaySubmitRetryable);
          submit_dag.fail(x23::DagFailureKind::Cancelled, "check readiness");
        }
        co_return;
      });

  auto hash_node =
      submit_dag.async<void>("hash transactions", submit_dag.after(decode_node),
                             [&, tx_hashes]() -> x23::Async<void> {
                               co_await hash_transactions(*txs, *tx_hashes);
                             });

  auto verify_node = submit_dag.async<AuthErrorCodeBatch>(
      "verify transactions", submit_dag.after(ready_node, hash_node),
      [&]() -> x23::Async<AuthErrorCodeBatch> {
        co_return co_await verify_submission(*txs, *commit_guard);
      });

  auto prune_node = submit_dag.async<void>(
      "prune auth failures", submit_dag.after(verify_node),
      [&]() -> x23::Async<void> {
        const AuthErrorCodeBatch &auth_ret = verify_node.get();
        // Auth is a trusted component and must return exactly one result per
        // submitted transaction; otherwise we cannot attribute auth results to
        // transactions and must not admit the batch.
        if (auth_ret.size() != txs->size()) {
          throw std::runtime_error(
              "authentication returned the wrong number of outcomes");
        }
        SignedTransactionBatch authenticated_txs;
        x23::Vec<size_t> authenticated_indices;
        TransactionHashBatch authenticated_hashes;
        authenticated_txs.reserve(txs->size());
        authenticated_indices.reserve(txs->size());
        authenticated_hashes.reserve(txs->size());
        size_t num_auth_failures = 0;
        for (size_t i = 0; i < auth_ret.size(); i++) {
          const AuthErrorCode &code = auth_ret[i];
          const size_t original_idx = (*original_indices)[i];
          const SchedGatewaySubmitOutcome outcome =
              submit_outcome_for_auth(code);
          if (outcome ==
              SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted) {
            authenticated_txs.push_back(std::move((*txs)[i]));
            authenticated_indices.push_back(original_idx);
            authenticated_hashes.push_back((*tx_hashes)[i]);
          }
          else {
            (*outcomes)[original_idx] = outcome;

            co_await scheduler_->record_transaction_outcome(
                /*tx_hash=*/(*tx_hashes)[i],
                /*status=*/transaction_status_for_auth(code),
                /*error_source=*/
                SchedulerTransactionErrorSource::SchedulerTransactionErrorAuth,
                /*error_code=*/static_cast<int32_t>(code));
            num_auth_failures++;
          }
        }
        MempoolMetrics::instance().auth_failed_tx_count.increment(
            num_auth_failures);
        *txs = std::move(authenticated_txs);
        *original_indices = std::move(authenticated_indices);
        *tx_hashes = std::move(authenticated_hashes);
        co_return;
      });

  auto capacity_node = submit_dag.async<void>(
      "enforce capacity", submit_dag.after(prune_node),
      [&]() -> x23::Async<void> {
        // Authentication must run first: a bad signing of a body that is
        // already pending is still rejected, while an authenticated duplicate
        // remains admitted without consuming another capacity slot.
        const ssize_t scheduler_size = scheduler_->size();
        const bool already_full = scheduler_size >= mempool_tx_size_limit_;
        size_t num_slots_left =
            already_full
                ? 0
                : static_cast<size_t>(mempool_tx_size_limit_ - scheduler_size);

        // If every authenticated entry fits even when they are all distinct,
        // there is no need to identify duplicates here. [add_txs()] already
        // deduplicates while adding the batch, and the scheduler can grow by
        // at most [txs->size()]. Besides avoiding unnecessary account-bucket
        // lookups, this leaves the parallel hashes above as the only hashes of
        // these transaction bodies.
        if (num_slots_left >= txs->size()) {
          co_return;
        }

        SignedTransactionBatch capacity_txs;
        x23::Vec<size_t> capacity_indices;
        TransactionHashBatch capacity_hashes;
        capacity_txs.reserve(txs->size());
        capacity_indices.reserve(txs->size());
        capacity_hashes.reserve(txs->size());
        std::unordered_set<TransactionHash, SipHash24Hasher>
            selected_new_hashes;
        size_t num_discarded = 0;
        for (size_t i = 0; i < txs->size(); i++) {
          const TransactionHash &tx_hash = (*tx_hashes)[i];
          const bool already_pending =
              selected_new_hashes.contains(tx_hash) ||
              scheduler_->is_pending((*txs)[i], tx_hash);
          if (already_pending || num_slots_left > 0) {
            // Keep an already-pending duplicate in both vectors so they remain
            // aligned and [add_txs()] can report it as admitted. It does not
            // consume another capacity slot or get enqueued again.
            if (!already_pending) {
              num_slots_left--;
              selected_new_hashes.insert(tx_hash);
            }
            capacity_txs.push_back(std::move((*txs)[i]));
            capacity_indices.push_back((*original_indices)[i]);
            capacity_hashes.push_back(tx_hash);
          }
          else {
            (*outcomes)[(*original_indices)[i]] =
                SchedGatewaySubmitOutcome::SchedGatewaySubmitRetryable;

            co_await scheduler_->record_transaction_outcome(
                tx_hash, /*status=*/
                SchedulerTransactionStatus::SchedulerTransactionRetry,
                /*error_source=*/
                SchedulerTransactionErrorSource::
                    SchedulerTransactionErrorScheduler,
                /*error_code=*/
                static_cast<int32_t>(
                    SchedulerErrorCode::SchedulerErrorCapacity));
            num_discarded++;
          }
        }

        if (num_discarded > 0) {
          if (already_full) {
            MempoolMetrics::instance().rejected_capacity_rpc_count.increment();
            info("submit_unvetted rejected new txs as the mempool has too many "
                 "pending txs");
          }
          else {
            MempoolMetrics::instance().discarded_capacity_tx_count.increment(
                num_discarded);
          }
        }
        *txs = std::move(capacity_txs);
        *original_indices = std::move(capacity_indices);
        *tx_hashes = std::move(capacity_hashes);
        co_return;
      });

  auto add_node = submit_dag.async<void>(
      "add txs", submit_dag.after(capacity_node),
      [&, commit_guard, original_indices, outcomes]() -> x23::Async<void> {
        // Careful: it is tempting to overlap the overhead of
        // [authmempool_verify_transactions()] with that of
        // [scheduler_->add_txs()] (and then after remove any unverified txs
        // from the scheduler). But this would make the txs visible to
        // nominate() and validate() before the txs have been verified.
        const SchedGatewaySubmitOutcomeBatch add_outcomes =
            co_await scheduler_->add_txs(*txs, *tx_hashes);
        CHECK_EQ(add_outcomes.size(), txs->size());
        CHECK_EQ(original_indices->size(), txs->size());
        SignedTransactionBatch admitted_txs;
        admitted_txs.reserve(txs->size());
        for (size_t i = 0; i < add_outcomes.size(); i++) {
          const SchedGatewaySubmitOutcome outcome = add_outcomes[i];
          (*outcomes)[(*original_indices)[i]] = outcome;
          if (outcome ==
              SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted) {
            admitted_txs.push_back(std::move((*txs)[i]));
          }
        }
        *txs = std::move(admitted_txs);
        MempoolMetrics::instance().accepted_unvetted_tx_count.increment(
            txs->size());
        // [commit_guard] is a smart pointer that has a [reset()] method just
        // like [x23::CoroGuard]. clang-tidy prefers that we dereference
        // [commit_guard] with [*] below rather than write
        // [commit_guard->reset()], just so that this is more readable.
        (*commit_guard).reset();
        co_return;
      });

  submit_dag.async<void>(
      "launch vetted notify", submit_dag.after(add_node),
      [&, txs]() -> x23::Async<void> {
        pending_detached_coroutines_.fetch_add(1, std::memory_order_acquire);
        [](MempoolCoordinatorRpc &coord,
           std::shared_ptr<SignedTransactionBatch> txs) -> x23::Detached {
          // We serialize and send the response in a detached coroutine so that
          // the admission RPC does not wait on the coordinator notification.
          ParallelBatch response;
          co_await t_to_parallel_batch(*txs, response);
          co_await coord.coord_client_->coordscheduler_notify_vetted(response);
          coord.pending_detached_coroutines_.fetch_sub(
              1, std::memory_order_release);
        }(*this, txs);
        co_return;
      });

  co_await submit_dag.run();
  if (!submit_dag.ok() && submit_dag.failure().exception) {
    std::rethrow_exception(submit_dag.failure().exception);
  }
  if (const char *dot_dir = std::getenv("MEMPOOL_DAG_DOT_DIR");
      dot_dir != nullptr && *dot_dir != '\0') {
    static std::atomic<size_t> dot_counter{0};
    const size_t dot_idx = dot_counter.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path dot_path =
        std::filesystem::path(dot_dir) /
        ("mempool_submit_unvetted_" + std::to_string(dot_idx) + ".dot");
    std::ofstream graph_dot(dot_path, std::ios::out | std::ios::trunc);
    graph_dot << submit_dag.dot();
  }
  co_return std::move(*outcomes);
}

x23::Async<void>
MempoolCoordinatorRpc::schedulercoord_submit_vetted(
    ValidatorID validator_id, ParallelSignedTransactionBatch batch)
{
  if (!ready()) {
    co_return;
  }
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
  auto txs = std::make_shared<SignedTransactionBatch>();
  auto tx_hashes = std::make_shared<TransactionHashBatch>();
  auto commit_guard = std::make_shared<x23::CoroGuard>();
  x23::Dag submit_dag("schedulercoord_submit_vetted", default_threadpool());

  auto decode_node =
      submit_dag.async<void>("decode batch", [&]() -> x23::Async<void> {
        // Not [mempool_tx_size_limit_]: [enforce capacity] below admits an
        // over-capacity batch up to the free slots rather than dropping it.
        if (!(co_await parallel_batch_to_t(batch, *txs, kNoBatchSizeLimit))) {
          MempoolMetrics::instance()
              .rejected_malformed_vetted_rpc_count.increment();
          info("submit_vetted rejected malformed transaction batch");
          submit_dag.fail(x23::DagFailureKind::Cancelled, "decode batch");
          co_return;
        }
        record_submitted_vetted(txs->size());
        co_return;
      });

  auto hash_node =
      submit_dag.async<void>("hash transactions", submit_dag.after(decode_node),
                             [&, tx_hashes]() -> x23::Async<void> {
                               co_await hash_transactions(*txs, *tx_hashes);
                             });

  auto verify_node = submit_dag.async<AuthErrorCodeBatch>(
      "verify transactions", submit_dag.after(hash_node),
      [&]() -> x23::Async<AuthErrorCodeBatch> {
        co_return co_await verify_submission(*txs, *commit_guard);
      });

  auto prune_node = submit_dag.async<void>(
      "prune auth failures", submit_dag.after(verify_node),
      [&]() -> x23::Async<void> {
        const AuthErrorCodeBatch &auth_ret = verify_node.get();
        CHECK_EQ(auth_ret.size(), txs->size());
        x23::Vec<size_t> to_remove_indices;
        for (size_t i = 0; i < auth_ret.size(); i++) {
          const AuthErrorCode &code = auth_ret[i];
          if (code != AuthErrorCode::kAuthSuccess) {
            co_await scheduler_->record_transaction_outcome(
                /*tx_hash=*/(*tx_hashes)[i],
                /*status=*/transaction_status_for_auth(code),
                /*error_source=*/
                SchedulerTransactionErrorSource::SchedulerTransactionErrorAuth,
                /*error_code=*/static_cast<int32_t>(code));
            to_remove_indices.push_back(i);
          }
        }
        MempoolMetrics::instance().auth_failed_tx_count.increment(
            to_remove_indices.size());
        if (!to_remove_indices.empty()) {
          remove_indices(/*remove_from=*/*txs, /*indices=*/to_remove_indices);
          remove_indices(/*remove_from=*/*tx_hashes,
                         /*indices=*/to_remove_indices);
        }
        co_return;
      });

  submit_dag.async<void>("add txs", submit_dag.after(prune_node),
                         [&, commit_guard]() -> x23::Async<void> {
                           co_await scheduler_->add_txs(*txs, *tx_hashes);
                           MempoolMetrics::instance()
                               .accepted_vetted_tx_count.increment(txs->size());
                           // [commit_guard] is a smart pointer that has a
                           // [reset()] method just like [x23::CoroGuard].
                           // clang-tidy prefers that we dereference
                           // [commit_guard] with [*] below rather than write
                           // [commit_guard->reset()], just so that this is more
                           // readable.
                           (*commit_guard).reset();
                           co_return;
                         });

  co_await submit_dag.run();
  if (!submit_dag.ok() && submit_dag.failure().exception) {
    std::rethrow_exception(submit_dag.failure().exception);
  }
  if (const char *dot_dir = std::getenv("MEMPOOL_DAG_DOT_DIR");
      dot_dir != nullptr && *dot_dir != '\0') {
    static std::atomic<size_t> dot_counter{0};
    const size_t dot_idx = dot_counter.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path dot_path =
        std::filesystem::path(dot_dir) /
        ("mempool_submit_vetted_" + std::to_string(dot_idx) + ".dot");
    std::ofstream graph_dot(dot_path, std::ios::out | std::ios::trunc);
    graph_dot << submit_dag.dot();
  }
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
MempoolCoordinatorRpc::eventsched_commit_block(BlockID block_id,
                                               const ConsensusValue &value)
{
  co_return co_await event_sched_client_->eventsched_commit_block(block_id,
                                                                  value);
}

x23::Async<EventBlockResult>
MempoolCoordinatorRpc::eventsched_close_block(BlockID block_id,
                                              BlockHeight block_height)
{
  co_return co_await event_sched_client_->eventsched_close_block(block_id,
                                                                 block_height);
}

x23::Async<DbBlockResult>
MempoolCoordinatorRpc::commit_snapshot(BlockID block_id, ConsensusValue value)
{
  co_return co_await db_sched_client_->dbsched_commit_snapshot(block_id, value);
}

x23::Async<void>
MempoolCoordinatorRpc::finish_block(BlockID block_id, size_t executor_idx)
{
  if (finish_executor_started_test_hook_) {
    co_await default_threadpool().bg();
    finish_executor_started_test_hook_(block_id, executor_idx);
  }
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
  std::exception_ptr first_exception;
  for (x23::Async<void> &handle : handles) {
    try {
      co_await handle;
    } catch (...) {
      if (!first_exception) {
        first_exception = std::current_exception();
      }
    }
  }
  if (first_exception) {
    std::rethrow_exception(first_exception);
  }
}

x23::Vec<x23::DagNode<void>>
MempoolCoordinatorRpc::add_finish_all_executors_nodes(
    x23::Dag &dag, const PendingBlock &cache, const x23::DagDeps &deps,
    const x23::Vec<x23::DagCleanup> &cleanups)
{
  CHECK_EQ(cleanups.size(), exec_clients_.size());
  x23::Vec<x23::DagNode<void>> nodes;
  nodes.reserve(exec_clients_.size());
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    nodes.push_back(
        dag.async<void>("finish executor", deps, [&, i]() -> x23::Async<void> {
          co_await finish_block(cache.block_id,
                                /*executor_idx=*/i);
          cleanups[i].disarm();
          co_return;
        }));
  }
  return nodes;
}

std::optional<SHA256Hash>
MempoolCoordinatorRpc::try_get_txset_hash(
    const x23::XdrVec<TransactionHash> &hashes)
{
  MerkleTriePrefix trie(/*allocator_type=*/Allocator<Node>::Type::kBump);
  WorkerPool &worker_pool = WorkerPool::get_default_pool();
  const size_t num_threads = available_parallelism();
  const std::vector<ChunkRange> chunks =
      make_chunks(hashes.size(), num_threads);

  std::atomic<bool> duplicate_hash{false};
  // JobScope joins every submitted insertion job before `trie` and `hashes`
  // can leave scope, even if a later submit() throws mid-loop. Without it, an
  // allocation failure while queuing a chunk would let this frame unwind while
  // an earlier worker still holds `&trie`, producing a use-after-return.
  {
    WorkerPool::JobScope scope(worker_pool);
    scope.reserve(chunks.size());
    for (const ChunkRange &chunk : chunks) {
      scope.submit([chunk, &trie, &hashes, &duplicate_hash] {
        for (size_t j = chunk.start; j < chunk.end; j++) {
          // We add each tx hash to the Merkle trie as a key with an empty
          // value. So in essence, we are using the Merkle trie as a set.
          //
          // The set insertion is itself the uniqueness check: a repeated hash
          // is the one input the trie rejects, so we report it to the caller
          // instead of running a separate dedup pass over [hashes].
          if (!trie.insert(HexArray(hashes[j]), std::span<const std::byte>())) {
            duplicate_hash.store(true, std::memory_order_relaxed);
            return;
          }
        }
      });
    }
    scope.wait();
  }
  if (duplicate_hash.load(std::memory_order_relaxed)) {
    return std::nullopt;
  }
  trie.generate_all_hashes(num_threads);
  return trie.get_root_hash();
}

SHA256Hash
MempoolCoordinatorRpc::get_txset_hash(
    const x23::XdrVec<TransactionHash> &hashes)
{
  const std::optional<SHA256Hash> txset_hash = try_get_txset_hash(hashes);
  // Callers of this overload hash a txset this node built itself, which the
  // mempool has already deduplicated. Use [try_get_txset_hash()] for hashes
  // that came off the wire.
  CHECK(txset_hash.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  return *txset_hash;
}

x23::Async<void>
MempoolCoordinatorRpc::propose_batch(
    BlockID block_id, x23::span<const SignedTransaction> txs,
    x23::XdrVec<NondeterministicProposalData> &transaction_nominations,
    x23::span<ExecResult> ret_chunk, size_t executor_idx)
{
  ExecProposeBatchResult res =
      co_await exec_clients_[executor_idx].executorsched_propose_batch(block_id,
                                                                       txs);
  if (propose_batch_result_test_hook_) {
    propose_batch_result_test_hook_(res);
  }
  CHECK_EQ(res.size(), txs.size());
  // Record nomination metadata for txs that succeeded on the executor and mark
  // the per-tx status in [ret_chunk] so upstream callers can rebuild summaries.
  for (size_t i = 0; i < res.size(); i++) {
    const ExecProposeResult &result = res[i];
    CHECK(result.result() != ExecResult::ExecResultUnrecoverable);
    ret_chunk[i] = result.result();
    if (tx_succeeded(ret_chunk[i])) {
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
    x23::span<ExecResult> ret)
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
    x23::span<ExecResult> ret_chunk(ret.begin() + chunk.start,
                                    ret.begin() + chunk.end);

    // We multiplex the small chunks across multiple TCP connections and
    // multiple executors by setting the [executor_idx] to [i %
    // exec_clients_.size()].
    handles.push_back(propose_batch(block_id, tx_chunk, nomination_chunks[i],
                                    ret_chunk,
                                    /*executor_idx=*/i % exec_clients_.size()));
  }

  // Wait for the executor to finish executing the txs.
  std::exception_ptr first_exception;
  for (size_t i = 0; i < chunks.size(); i++) {
    try {
      co_await handles[i];
    } catch (...) {
      if (!first_exception) {
        first_exception = std::current_exception();
      }
    }
  }
  if (first_exception) {
    std::rethrow_exception(first_exception);
  }
}

x23::Async<void>
MempoolCoordinatorRpc::propose_batch_in_chunks(
    BlockID block_id, x23::span<SignedTransaction> txs,
    x23::XdrVec<NondeterministicProposalData> &transaction_nominations,
    x23::span<ExecResult> ret)
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
    // The executor outcome for each tx is written into this span.
    x23::span<ExecResult> status_chunk(ret.begin() + chunk.start,
                                       ret.begin() + chunk.end);

    handles.push_back(multiplex_large_chunk_across_executors(
        block_id, tx_chunk, nomination_chunks[i], status_chunk));
  }

  transaction_nominations.reserve(txs.size());
  // Wait for all RPC responses before reading their output buffers. If one
  // fails, the other coroutines still reference those buffers and spans.
  std::exception_ptr first_exception;
  for (size_t i = 0; i < large_chunks.size(); i++) {
    try {
      co_await handles[i];
    } catch (...) {
      if (!first_exception) {
        first_exception = std::current_exception();
      }
    }
  }
  if (first_exception) {
    std::rethrow_exception(first_exception);
  }
  for (const auto &chunks : nomination_chunks) {
    for (const x23::XdrVec<NondeterministicProposalData> &nominated_txs :
         chunks) {
      transaction_nominations += nominated_txs;
    }
  }
}

x23::Async<ExecResult>
MempoolCoordinatorRpc::open_proposal_block(
    BlockID block_id, BlockExecutionContext block_execution,
    size_t executor_idx)
{
  return exec_clients_[executor_idx].executorsched_open_proposal_block(
      block_id, block_execution);
}

MempoolCoordinatorRpc::InitializeBlockNodes
MempoolCoordinatorRpc::add_initialize_block_nodes(
    x23::Dag &dag, const PendingBlock &cache, const x23::DagDeps &deps,
    const x23::DagCleanup &snapshot_cleanup,
    const x23::DagCleanup &event_cleanup,
    const x23::Vec<x23::DagCleanup> &executor_cleanups)
{
  CHECK_EQ(executor_cleanups.size(), exec_clients_.size());
  InitializeBlockNodes nodes;
  nodes.create_snapshot = dag.async<DbSchedResult>(
      "create proposal snapshot", deps, [&]() -> x23::Async<DbSchedResult> {
        const DbSchedResult result = co_await create_snapshot(
            /*prev_block_id=*/last_committed_block_.id, cache.block_id,
            BlockExecutionMode::kBlockExecutionModeProposal);
        if (result == DbSchedResult::DbSchedResultOK) {
          snapshot_cleanup.arm();
        }
        co_return result;
      });

  // Executor proposal blocks depend on the event block being open because they
  // can send event RPCs as soon as they start processing.
  nodes.open_event =
      dag.async<RPCError>("open event block", deps,
                          [&]() -> x23::Async<RPCError> {
                            const RPCError result =
                                co_await eventsched_open_block(cache.block_id);
                            if (result == RPCError::kRPCNone) {
                              event_cleanup.arm();
                            }
                            co_return result;
                          });

  nodes.open_proposal_blocks.reserve(exec_clients_.size());
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    nodes.open_proposal_blocks.push_back(dag.async<ExecResult>(
        "open executor proposal block", dag.after(nodes.open_event),
        [&, i]() -> x23::Async<ExecResult> {
          const ExecResult result =
              co_await open_proposal_block(cache.block_id,
                                           cache.block_execution,
                                           /*executor_idx=*/i);
          if (result == ExecResult::ExecResultOK) {
            executor_cleanups[i].arm();
          }
          co_return result;
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
    x23::Vec<size_t> handle_end_indices;
    x23::Vec<ScheduledChunk> scheduled_chunks;
    x23::Vec<SummarizedChunk> summarized_chunks;
    x23::Vec<x23::XdrVec<TransactionHash>> txset_hash_chunks;
    size_t num_tx_outcomes_processed{0};
    size_t tx_write_idx{0};
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
  nodes.serialize_chunks.reserve(kNumSchedulerIterations * 3);

  nodes.nomination_cleanup = dag.cleanup(
      "rollback scheduler nomination", [&, state]() -> x23::Async<void> {
        scheduler_->reenqueue_txs(cache.tx_hashes.begin(),
                                  cache.tx_hashes.end());
        {
          x23::CoroGuard scheduler_guard;
          co_await scheduler_guard(scheduler_->global_lock_);
          co_await scheduler_->abandon_pending_nomination(cache.block_id);
        }
        cache.block_id = 0;
        co_return;
      });
  const x23::DagCleanup nomination_cleanup = nodes.nomination_cleanup;

  nodes.start_nomination =
      dag.async<void>("start scheduler nomination", std::move(scheduling_deps),
                      [&, nomination_cleanup]() -> x23::Async<void> {
                        x23::CoroGuard scheduler_guard;
                        co_await scheduler_guard(scheduler_->global_lock_);
                        scheduler_->start_pending_nomination(cache.block_id);
                        nomination_cleanup.arm();
                        co_return;
                      });

  x23::DagNode<void> previous_schedule_node;
  for (size_t i = 0; i < kNumSchedulerIterations; i++) {
    x23::DagDeps deps = i == 0 ? dag.after(nodes.start_nomination)
                               : dag.after(previous_schedule_node);
    auto schedule_node =
        dag.async<void>("schedule nomination chunk", std::move(deps),
                        [&, state, i]() -> x23::Async<void> {
                          if (state->stopped) {
                            co_return;
                          }
                          {
                            x23::CoroGuard scheduler_guard;
                            co_await scheduler_guard(scheduler_->global_lock_);
                            co_await scheduler_->schedule(
                                kMaxBlockSize / kNumSchedulerIterations,
                                cache.tx_hashes, cache.txs);
                          }
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
          x23::span<ExecResult> ret_span(cache.statuses.begin() + start,
                                         cache.statuses.begin() + end);
          // [schedule()] appends transactions to the end of [txs]. We reserve
          // [txs] up front in [schedulercoord_propose_block()], so spans into
          // it do not become invalid when later schedule nodes append more txs.
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
            cache.failed_tx_hashes.clear();
            cache.failed_tx_results.clear();
            CHECK(cache.result_tx_nominations.empty());
          }

          if (state->scheduled_chunks[i].active) {
            const size_t result_start = cache.result_tx_hashes.size();
            x23::XdrVec<NondeterministicProposalData> &chunk_nominations =
                cache.transaction_nomination_chunks[i];
            cache.result_tx_nominations.append(
                std::make_move_iterator(chunk_nominations.begin()),
                std::make_move_iterator(chunk_nominations.end()));

            // We need to remove failed txs from [cache.txs],
            // so [tx_write_idx] tracks where to overwrite
            // successful txs to in [cache.txs].
            for (size_t tx_idx = state->num_tx_outcomes_processed;
                 tx_idx < state->handle_end_indices[i]; tx_idx++) {
              if (tx_succeeded(cache.statuses[tx_idx])) {
                const TransactionHash &tx_hash = cache.tx_hashes[tx_idx];
                cache.result_tx_hashes.push_back(tx_hash);
                state->txset_hash_chunks[i].push_back(tx_hash);
                if (state->tx_write_idx != tx_idx) {
                  cache.txs[state->tx_write_idx] = std::move(cache.txs[tx_idx]);
                }
                state->tx_write_idx++;
              }
              else {
                cache.failed_tx_hashes.push_back(cache.tx_hashes[tx_idx]);
                cache.failed_tx_results.push_back(cache.statuses[tx_idx]);
              }
            }
            state->num_tx_outcomes_processed = state->handle_end_indices[i];
            state->summarized_chunks[i] = SummarizedChunk{
                .start = result_start,
                .end = cache.result_tx_hashes.size(),
                .active = true,
            };
          }

          if (i + 1 == kNumSchedulerIterations) {
            CHECK_EQ(state->num_tx_outcomes_processed, cache.tx_hashes.size());
            CHECK_EQ(cache.result_tx_hashes.size(),
                     cache.result_tx_nominations.size());
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
          const x23::XdrVec<TransactionHash> &tx_hashes =
              state->txset_hash_chunks[i];
          WorkerPool &worker_pool = WorkerPool::get_default_pool();
          const size_t num_threads = std::max(available_parallelism() / 2, 1UL);
          const std::vector<ChunkRange> chunks =
              make_chunks(tx_hashes.size(), num_threads);
          // JobScope joins every insertion job before `tx_hashes` leaves scope,
          // even if a later submit() throws mid-loop.
          WorkerPool::JobScope scope(worker_pool);
          scope.reserve(chunks.size());
          for (const ChunkRange &chunk : chunks) {
            scope.submit([chunk, &tx_hashes, txset_trie] {
              for (size_t j = chunk.start; j < chunk.end; j++) {
                CHECK(txset_trie->insert(HexArray(tx_hashes[j]),
                                         std::span<const std::byte>()));
              }
            });
          }
          scope.wait();
          return;
        });
    nodes.txset_insert_chunks.push_back(txset_insert_node);
  }

  x23::DagNode<void> previous_hash_serialize_node;
  x23::DagNode<void> previous_nomination_serialize_node;
  x23::DagNode<void> previous_txs_serialize_node;
  for (size_t i = 0; i < kNumSchedulerIterations; i++) {
    x23::DagDeps hash_serialize_deps =
        i == 0 ? dag.after(summary_nodes[i])
               : dag.after(summary_nodes[i], previous_hash_serialize_node);
    auto hash_serialize_node = dag.async<void>(
        "serialize nomination tx hashes", std::move(hash_serialize_deps),
        [&, state, i]() -> x23::Async<void> {
          Proposal &proposal = cache.proposal_result.proposed_block().proposal;
          if (i == 0) {
            proposal.transactions.hash = {};
          }
          if (!state->summarized_chunks[i].active) {
            co_return;
          }
          const size_t start = state->summarized_chunks[i].start;
          const size_t end = state->summarized_chunks[i].end;
          x23::span<const TransactionHash> tx_hashes_span(
              cache.result_tx_hashes.begin() + start,
              cache.result_tx_hashes.begin() + end);
          co_await append_to_parallel_batch(tx_hashes_span,
                                            proposal.transactions.hash);
          co_return;
        });
    nodes.serialize_chunks.push_back(hash_serialize_node);
    previous_hash_serialize_node = hash_serialize_node;

    x23::DagDeps nomination_serialize_deps =
        i == 0
            ? dag.after(summary_nodes[i])
            : dag.after(summary_nodes[i], previous_nomination_serialize_node);
    auto nomination_serialize_node = dag.async<void>(
        "serialize nomination nondeterministic data",
        std::move(nomination_serialize_deps),
        [&, state, i]() -> x23::Async<void> {
          Proposal &proposal = cache.proposal_result.proposed_block().proposal;
          if (i == 0) {
            proposal.transactions.nondeterministic_data = {};
          }
          if (!state->summarized_chunks[i].active) {
            co_return;
          }
          const size_t start = state->summarized_chunks[i].start;
          const size_t end = state->summarized_chunks[i].end;
          x23::span<const NondeterministicProposalData> nominations_span(
              cache.result_tx_nominations.begin() + start,
              cache.result_tx_nominations.begin() + end);
          co_await append_to_parallel_batch(
              nominations_span, proposal.transactions.nondeterministic_data);
          co_return;
        });
    nodes.serialize_chunks.push_back(nomination_serialize_node);
    previous_nomination_serialize_node = nomination_serialize_node;

    x23::DagDeps txs_serialize_deps =
        i == 0 ? dag.after(summary_nodes[i])
               : dag.after(summary_nodes[i], previous_txs_serialize_node);
    auto txs_serialize_node = dag.async<void>(
        "serialize nominated txs", std::move(txs_serialize_deps),
        [&, state, i]() -> x23::Async<void> {
          if (i == 0) {
            cache.proposal_result.proposed_block().txs = {};
          }
          if (!state->summarized_chunks[i].active) {
            co_return;
          }
          const size_t start = state->summarized_chunks[i].start;
          const size_t end = state->summarized_chunks[i].end;
          x23::span<const SignedTransaction> txs_span(cache.txs.begin() + start,
                                                      cache.txs.begin() + end);
          co_await append_to_parallel_batch(
              txs_span, cache.proposal_result.proposed_block().txs);
          co_return;
        });
    nodes.serialize_chunks.push_back(txs_serialize_node);
    previous_txs_serialize_node = txs_serialize_node;
  }
  nodes.resize_txs =
      dag.async<void>("resize nominated txs", dag.after(nodes.summarize),
                      [&, state]() -> x23::Async<void> {
                        cache.txs.resize(state->tx_write_idx);
                        co_return;
                      });
  return nodes;
}

MempoolCoordinatorRpc::FinishNominationNodes
MempoolCoordinatorRpc::add_finish_nominating_block_nodes(
    x23::Dag &dag, const DbTip &db_tip, const ProposalContext &context,
    PendingBlock &cache, const x23::DagDeps &proposal_deps,
    const x23::DagDeps &proposal_txs_deps,
    const x23::Vec<x23::DagCleanup> &executor_cleanups,
    const std::shared_ptr<MerkleTriePrefix> &txset_trie,
    const x23::DagDeps &txset_hash_deps)
{
  FinishNominationNodes nodes;
  nodes.finish_executors =
      add_finish_all_executors_nodes(dag, cache, proposal_deps,
                                     executor_cleanups);
  nodes.close_snapshot =
      dag.async<DbBlockResult>("close proposal snapshot", proposal_deps,
                               [&]() -> x23::Async<DbBlockResult> {
                                 co_return co_await close_snapshot(
                                     cache.block_id);
                               });
  nodes.proposal_txs_hash = dag.cpu<ProposalTxsHash>(
      "compute proposal txs hash", proposal_txs_deps, [&cache] {
        return proposal_txs_compatibility_hash(
            cache.proposal_result.proposed_block().proposal.transactions);
      });
  nodes.close_event = dag.async<EventBlockResult>(
      "close event block", proposal_deps,
      [&]() -> x23::Async<EventBlockResult> {
        co_return co_await eventsched_close_block(
            cache.block_id, db_tip.value.proposal_header.block_height + 1);
      });
  nodes.txset_hash =
      dag.cpu<SHA256Hash>("compute txset hash", txset_hash_deps, [txset_trie] {
        const size_t num_threads = available_parallelism();
        txset_trie->generate_all_hashes(num_threads);
        return txset_trie->get_root_hash();
      });
  nodes.build_header = dag.async<void>(
      "build nomination header",
      dag.after(nodes.close_snapshot, nodes.close_event, nodes.txset_hash),
      [&, close_snapshot = nodes.close_snapshot,
       close_event = nodes.close_event,
       txset_hash = nodes.txset_hash]() -> x23::Async<void> {
        {
          DbBlockResult result = close_snapshot.get();
          CHECK_EQ(result.result(), DbSchedResult::DbSchedResultOK);
          cache.db_tip.value.proposal_header.storage_hash =
              result.block().storage_hash;
          cache.db_tip.storage_long_hash = result.block().storage_long_hash;
          cache.db_tip.value.proposal_header.config_hash =
              compute_config_hash(result.block().config_hash_preimage);
        }
        {
          EventBlockResult result = close_event.get();
          CHECK_EQ(result.error(), RPCError::kRPCNone);
          cache.db_tip.value.proposal_header.events_hash = result.root_hash();
        }
        Proposal &proposal = cache.proposal_result.proposed_block().proposal;
        proposal.header = ProposalHeader{
            .ext = {},
            .network_hash = network_hash_,
            .config_hash = cache.db_tip.value.proposal_header.config_hash,
            .proposer = context.proposer,
            .block_timepoint = context.block_timepoint,
            .block_height = db_tip.value.proposal_header.block_height + 1,
            .txs_hash = txset_hash.get(),
            .storage_hash = cache.db_tip.value.proposal_header.storage_hash,
            .events_hash = cache.db_tip.value.proposal_header.events_hash};
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

x23::Async<MempoolCoordinatorRpc::JointCommitResult>
MempoolCoordinatorRpc::commit_block(const PendingBlock &cache)
{
  x23::Async<DbBlockResult> commit_snapshot_handle =
      commit_snapshot(cache.block_id, cache.db_tip.value);
  std::optional<x23::Async<RPCError>> event_commit_handle;
  std::exception_ptr first_exception;
  try {
    event_commit_handle.emplace(
        eventsched_commit_block(cache.block_id, cache.db_tip.value));
  } catch (...) {
    first_exception = std::current_exception();
  }
  JointCommitResult result;
  // Keep launch and awaits separate so a failure cannot skip draining a
  // sibling operation that has already started.
  try {
    result.db = co_await commit_snapshot_handle;
  } catch (...) {
    if (!first_exception) {
      first_exception = std::current_exception();
    }
  }
  if (event_commit_handle) {
    try {
      result.event_commit = co_await *event_commit_handle;
    } catch (...) {
      if (!first_exception) {
        first_exception = std::current_exception();
      }
    }
  }
  if (first_exception) {
    std::rethrow_exception(first_exception);
  }
  co_return result;
}

x23::Async<SchedulerProposalResult>
MempoolCoordinatorRpc::schedulercoord_propose_block(BlockID parent_id,
                                                    ProposalContext context)
{
  while (!ready()) {
    co_await ready_waiters_.wait(false);
  }
  if (context.protocol != 0) {
    co_return SchedulerProposalResult(
        SchedulerProposalOutcome::SchedulerProposalError);
  }

  ElapsedTimer timer;
  NominationTelemetry nomination_telemetry;
  x23::Defer record_nomination_stats([&] {
    nomination_telemetry.duration = timer.elapsed();
    record_metric_cleanup(
        [&] { record_nomination_attempt(nomination_telemetry); });
  });
  x23::CoroGuard cg;
  co_await cg(nominated_block_cache_lock_);

  if (last_committed_block_.id != parent_id) {
    info("schedulercoord_nominate ret");
    co_return SchedulerProposalResult(
        SchedulerProposalOutcome::SchedulerProposalError);
  }
  if (last_committed_block_.value.proposal_header.block_height < 1) {
    // Heights 0 and 1 are reserved for genesis initialization and the genesis
    // ceremony. Ordinary nomination starts at height 2.
    info("schedulercoord_nominate ret");
    co_return SchedulerProposalResult(
        SchedulerProposalOutcome::SchedulerProposalError);
  }
  // The next block must have a [block_timepoint] that is >= the last committed
  // block's timepoint.
  if (context.block_timepoint <
      last_committed_block_.value.proposal_header.block_timepoint) {
    error("Refusing to nominate a block at timepoint {}: the committed tip is "
          "at {}",
          context.block_timepoint,
          last_committed_block_.value.proposal_header.block_timepoint);
    co_return SchedulerProposalResult(
        SchedulerProposalOutcome::SchedulerProposalError);
  }
  if (nominated_block_cache_.block_id) {
    // A block was nominated by this node and not yet committed or abandoned.
    // [schedulercoord_commit_block()] needs to be called before this node can
    // nominate another block.
    info("schedulercoord_nominate ret");
    co_return SchedulerProposalResult(
        SchedulerProposalOutcome::SchedulerProposalError);
  }

  std::atomic<BlockID> proposal_block_id{0};
  x23::Dag nomination_dag("schedulercoord_nominate", default_threadpool());
  x23::DagCleanup cache_cleanup =
      nomination_dag.cleanup("clear proposal cache", [&]() -> x23::Async<void> {
        nominated_block_cache_.block_id = 0;
        co_return;
      });
  x23::DagCleanup snapshot_cleanup = nomination_dag.cleanup(
      "abandon proposal snapshot", [&]() -> x23::Async<void> {
        CHECK_EQ(co_await abandon_snapshot(proposal_block_id.load()),
                 DbSchedResult::DbSchedResultOK);
        co_return;
      });
  x23::DagCleanup event_cleanup = nomination_dag.cleanup(
      "abandon proposal event block", [&]() -> x23::Async<void> {
        CHECK_EQ(co_await eventsched_abandon_block(proposal_block_id.load()),
                 RPCError::kRPCNone);
        co_return;
      });
  x23::Vec<x23::DagCleanup> executor_cleanups;
  executor_cleanups.reserve(exec_clients_.size());
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    executor_cleanups.push_back(
        nomination_dag.cleanup("finish proposal executor",
                               [&, i]() -> x23::Async<void> {
                                 co_await finish_block(proposal_block_id.load(),
                                                       /*executor_idx=*/i);
                                 co_return;
                               }));
  }

  // HOG transfers and auth changes from the previous block both affect the
  // scheduling priorities for this block, but neither depends on the other.
  auto transfer_data_node = nomination_dag.async<void>(
      "wait transfer data", [&]() -> x23::Async<void> {
        co_await wait_for_transfer_data(
            last_committed_block_.value.proposal_header.block_height);
        co_return;
      });
  auto auth_data_node =
      nomination_dag.async<void>("wait auth data", [&]() -> x23::Async<void> {
        co_await wait_for_auth_data(
            last_committed_block_.value.proposal_header.block_height);
        co_return;
      });

  auto prepare_cache_node = nomination_dag.async<void>(
      "prepare nomination cache", [&]() -> x23::Async<void> {
        nominated_block_cache_.block_id =
            BlockIdGenerator::generate(BlockIdLane::kMempool);
        // Fixed before execution; every executor gets this same value.
        nominated_block_cache_.block_execution = BlockExecutionContext{
            .block_height =
                last_committed_block_.value.proposal_header.block_height + 1,
            .block_timepoint = context.block_timepoint,
        };
        proposal_block_id.store(nominated_block_cache_.block_id);
        cache_cleanup.arm();
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
        // [nominated_block_cache_.proposal_result.proposed_block()]. We
        // fill in [proposed_block()] throughout this function, so we want our
        // writes to it preserved.
        nominated_block_cache_.proposal_result.outcome(
            SchedulerProposalOutcome::SchedulerProposalOK);
        co_return;
      });

  InitializeBlockNodes initialize_block_nodes =
      add_initialize_block_nodes(nomination_dag, nominated_block_cache_,
                                 nomination_dag.after(transfer_data_node,
                                                      auth_data_node,
                                                      prepare_cache_node),
                                 snapshot_cleanup, event_cleanup,
                                 executor_cleanups);
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
  FinishNominationNodes finish_nodes = add_finish_nominating_block_nodes(
      nomination_dag, last_committed_block_, context, nominated_block_cache_,
      dag_deps_from_nodes(schedule_nodes.propose_chunks),
      dag_deps_from_nodes(schedule_nodes.serialize_chunks), executor_cleanups,
      schedule_nodes.txset_trie,
      dag_deps_from_nodes(schedule_nodes.txset_insert_chunks));

  co_await nomination_dag.run();
  if (nomination_dag.ok()) {
    schedule_nodes.nomination_cleanup.disarm();
    cache_cleanup.disarm();
    snapshot_cleanup.disarm();
    event_cleanup.disarm();
  }
  for (const x23::DagFailure &failure : nomination_dag.cleanup_failures()) {
    try {
      std::rethrow_exception(failure.exception);
    } catch (const std::exception &e) {
      error("scheduler proposal cleanup '{}' failed: {}", failure.node,
            e.what());
    } catch (...) {
      error("scheduler proposal cleanup '{}' failed", failure.node);
    }
  }
  if (!nomination_dag.ok() && nomination_dag.failure().exception) {
    std::rethrow_exception(nomination_dag.failure().exception);
  }
  if (const char *dot_dir = std::getenv("MEMPOOL_DAG_DOT_DIR");
      dot_dir != nullptr && *dot_dir != '\0') {
    static std::atomic<size_t> dot_counter{0};
    const size_t dot_idx = dot_counter.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path dot_path =
        std::filesystem::path(dot_dir) /
        ("mempool_" + std::to_string(dot_idx) + ".dot");
    std::ofstream graph_dot(dot_path, std::ios::out | std::ios::trunc);
    graph_dot << nomination_dag.dot();
  }

  const bool initialized_block = initialize_block_nodes.initialized.get();
  if (!initialized_block) {
    info("schedulercoord_nominate ret");
    co_return SchedulerProposalResult(
        SchedulerProposalOutcome::SchedulerProposalError);
  }

  // We set [nominated_block_cache_.proposal_result.outcome()] to
  // [SchedulerProposalOutcome::SchedulerProposalOK] above. See the comments
  // there.
  nominated_block_cache_.proposal_result.proposed_block().block_id =
      nominated_block_cache_.block_id;
  nomination_telemetry.succeeded = true;
  nomination_telemetry.scheduled_tx_count =
      nominated_block_cache_.tx_hashes.size();
  nomination_telemetry.nominated_tx_count =
      nominated_block_cache_.result_tx_hashes.size();
  nomination_telemetry.failed_tx_count =
      nominated_block_cache_.failed_tx_hashes.size();
  const Proposal &proposal =
      nominated_block_cache_.proposal_result.proposed_block().proposal;
  ConsensusValue value{.protocol = 0,
                       .ext = {},
                       .prev_hash = sha256(last_committed_block_.value),
                       .proposal_header = proposal.header,
                       .proposal_txs_hash =
                           finish_nodes.proposal_txs_hash.get(),
                       .upgrades = {}};
  // default_wallet and network_config come from the committed parent tip: the
  // cache's db_tip only ever receives storage/event hashes from the
  // scheduler's close result, never the chain-configuration fields.
  nominated_block_cache_.db_tip = DbTip{
      .id = nominated_block_cache_.proposal_result.proposed_block().block_id,
      .storage_long_hash = nominated_block_cache_.db_tip.storage_long_hash,
      .value = std::move(value),
      .default_wallet = last_committed_block_.default_wallet,
      .network_config = last_committed_block_.network_config};
  info("schedulercoord_nominate ret");
  co_return nominated_block_cache_.proposal_result;
}

x23::Async<GenesisCeremonyResult>
MempoolCoordinatorRpc::schedulercoord_commit_genesis_ceremony(
    GenesisCeremonyRequest request)
{
  // Loading height 0 can complete before the event server and executors have
  // registered.  The coordinator is allowed to submit the ceremony as soon as
  // it observes that tip, so wait for the runtime just like ordinary block
  // proposal does instead of treating this startup race as a fatal invariant.
  while (!ready()) {
    co_await ready_waiters_.wait(false);
  }
  auto result = [](GenesisCeremonyOutcome outcome) {
    return GenesisCeremonyResult(outcome);
  };

  if (request.context.protocol != 0 || request.expected_parent_height != 0) {
    co_return result(GenesisCeremonyOutcome::GenesisCeremonyBatchMismatch);
  }
  x23::XdrVec<SignedTransaction> txs;
  if (!(co_await parallel_batch_to_t(request.transactions, txs,
                                     kMaxBlockSize))) {
    co_return result(GenesisCeremonyOutcome::GenesisCeremonyInvalidTransaction);
  }
  for (const SignedTransaction &tx : txs) {
    if (tx.tx.invocation.invoked_address != kGenesisRootContractAddress) {
      co_return result(
          GenesisCeremonyOutcome::GenesisCeremonyInvalidTransaction);
    }
  }

  // Wait for the height-0 auth update and verify the transactions without
  // holding either coordinator lock. The auth_data_changed() callback needs
  // commit_lock_. Check the parent before waiting, then check it again under
  // the nomination lock below so a concurrent ceremony cannot make this stale
  // verification usable.
  {
    x23::CoroGuard cache_guard;
    co_await cache_guard(nominated_block_cache_lock_);
    const ConsensusValueHash parent_hash = sha256(last_committed_block_.value);
    if (last_committed_block_.value.proposal_header.block_height != 0 ||
        parent_hash != request.expected_parent_value_hash) {
      co_return result(GenesisCeremonyOutcome::GenesisCeremonyParentMismatch);
    }
    if (nominated_block_cache_.block_id != 0) {
      co_return result(GenesisCeremonyOutcome::GenesisCeremonyExecutionFailed);
    }
  }

  co_await wait_for_auth_data(/*expected_height=*/0);

  AuthErrorCodeBatch auth_results =
      co_await auth_client_->authmempool_verify_transactions(0, txs);
  if (auth_results.size() != txs.size() ||
      std::ranges::any_of(auth_results, [](AuthErrorCode auth_result) {
        return auth_result != AuthErrorCode::kAuthSuccess;
      })) {
    co_return result(GenesisCeremonyOutcome::GenesisCeremonyInvalidTransaction);
  }

  BlockID block_id = 0;
  ProposalTxs committed_proposal_txs;
  ConsensusValue value;
  {
    x23::CoroGuard cache_guard;
    co_await cache_guard(nominated_block_cache_lock_);

    // Ordinary commits hold the nomination-cache lock while updating
    // last_committed_block_. Revalidate after acquiring that same lock, so a
    // height-1 commit that completed while this request was being decoded
    // cannot be overwritten by a second height-1 block. Keeping cache_guard
    // until the ceremony nomination is ready prevents another commit from
    // advancing the tip after this check and before the block is reserved in
    // the cache. Do not also take commit_lock_ here: the asynchronous height-0
    // auth callback can hold it while genesis ceremony execution is pending.
    const ConsensusValueHash parent_hash = sha256(last_committed_block_.value);
    if (last_committed_block_.value.proposal_header.block_height != 0 ||
        parent_hash != request.expected_parent_value_hash) {
      co_return result(GenesisCeremonyOutcome::GenesisCeremonyParentMismatch);
    }

    if (nominated_block_cache_.block_id != 0) {
      co_return result(GenesisCeremonyOutcome::GenesisCeremonyExecutionFailed);
    }

    block_id = BlockIdGenerator::generate(BlockIdLane::kMempool);
    {
      x23::CoroGuard scheduler_guard;
      co_await scheduler_guard(scheduler_->global_lock_);
      scheduler_->start_pending_nomination(block_id);
    }

    bool snapshot_open = false;
    bool event_open = false;
    size_t executors_open = 0;
    auto cleanup = [&]() -> x23::Async<void> {
      for (size_t i = 0; i < executors_open; ++i) {
        co_await finish_block(block_id, i);
      }
      if (event_open) {
        CHECK_EQ(co_await eventsched_abandon_block(block_id),
                 RPCError::kRPCNone);
      }
      if (snapshot_open) {
        CHECK_EQ(co_await abandon_snapshot(block_id),
                 DbSchedResult::DbSchedResultOK);
      }
      {
        x23::CoroGuard scheduler_guard;
        co_await scheduler_guard(scheduler_->global_lock_);
        co_await scheduler_->abandon_pending_nomination(block_id);
      }
      nominated_block_cache_.block_id = 0;
      co_return;
    };

    if (co_await create_snapshot(
            last_committed_block_.id, block_id,
            BlockExecutionMode::kBlockExecutionModeProposal) !=
        DbSchedResult::DbSchedResultOK) {
      co_await cleanup();
      co_return result(GenesisCeremonyOutcome::GenesisCeremonyExecutionFailed);
    }
    snapshot_open = true;
    if (co_await eventsched_open_block(block_id) != RPCError::kRPCNone) {
      co_await cleanup();
      co_return result(GenesisCeremonyOutcome::GenesisCeremonyExecutionFailed);
    }
    event_open = true;
    // Same construction as an ordinary proposal.
    const BlockExecutionContext genesis_block_execution{
        .block_height =
            last_committed_block_.value.proposal_header.block_height + 1,
        .block_timepoint = request.context.block_timepoint,
    };
    for (size_t i = 0; i < exec_clients_.size(); ++i) {
      if (co_await open_proposal_block(block_id, genesis_block_execution, i) !=
          ExecResult::ExecResultOK) {
        co_await cleanup();
        co_return result(
            GenesisCeremonyOutcome::GenesisCeremonyExecutionFailed);
      }
      ++executors_open;
    }

    PendingBlock &cache = nominated_block_cache_;
    cache.block_id = block_id;
    cache.txs = std::move(txs);
    cache.tx_hashes.clear();
    cache.tx_hashes.reserve(cache.txs.size());
    for (const SignedTransaction &tx : cache.txs) {
      cache.tx_hashes.push_back(sha256(tx.tx));
    }
    cache.result_tx_hashes = cache.tx_hashes;
    cache.result_tx_nominations.clear();
    cache.failed_tx_hashes.clear();
    cache.failed_tx_results.clear();
    x23::span<ExecResult> ceremony_statuses(cache.statuses.begin(),
                                            cache.txs.size());
    std::ranges::fill(ceremony_statuses, ExecResult::ExecResultSystemError);
    co_await propose_batch_in_chunks(block_id, cache.txs,
                                     cache.result_tx_nominations,
                                     ceremony_statuses);
    if (std::ranges::any_of(ceremony_statuses, tx_failed) ||
        cache.result_tx_nominations.size() != cache.txs.size()) {
      co_await cleanup();
      co_return result(GenesisCeremonyOutcome::GenesisCeremonyExecutionFailed);
    }

    co_await finish_all_executors(block_id);
    executors_open = 0;
    ProposalTxs ceremony_proposal_txs;
    co_await t_to_parallel_batch(cache.result_tx_hashes,
                                 ceremony_proposal_txs.hash);
    co_await t_to_parallel_batch(cache.result_tx_nominations,
                                 ceremony_proposal_txs.nondeterministic_data);
    const DbBlockResult db_close = co_await close_snapshot(block_id);
    const EventBlockResult event_close =
        co_await eventsched_close_block(block_id, 1);
    if (db_close.result() != DbSchedResult::DbSchedResultOK ||
        event_close.error() != RPCError::kRPCNone) {
      co_await cleanup();
      co_return result(GenesisCeremonyOutcome::GenesisCeremonyExecutionFailed);
    }

    cache.proposal_result.outcome(
        SchedulerProposalOutcome::SchedulerProposalOK);
    SchedulerProposedBlock &proposed = cache.proposal_result.proposed_block();
    proposed.block_id = block_id;
    co_await t_to_parallel_batch(cache.txs, proposed.txs);
    // ParallelBatch contains nested x23::Vec members.  Keep each owner valid
    // until its normal destructor runs: GCC's optimized coroutine lowering can
    // otherwise reuse the moved-from frame slot before emitting its cleanup.
    // This path runs only once at genesis, so the copies are immaterial.
    proposed.proposal.transactions = ceremony_proposal_txs;
    proposed.proposal.header = ProposalHeader{
        .ext = {},
        .network_hash = network_hash_,
        .config_hash =
            compute_config_hash(db_close.block().config_hash_preimage),
        .proposer = request.context.proposer,
        .block_timepoint = request.context.block_timepoint,
        .block_height = 1,
        .txs_hash = get_txset_hash(cache.result_tx_hashes),
        .storage_hash = db_close.block().storage_hash,
        .events_hash = event_close.root_hash(),
    };
    committed_proposal_txs = proposed.proposal.transactions;
    value = ConsensusValue{
        .protocol = 0,
        .ext = {},
        .prev_hash = parent_hash,
        .proposal_header = proposed.proposal.header,
        .proposal_txs_hash =
            proposal_txs_compatibility_hash(committed_proposal_txs),
        .upgrades = {},
    };
    cache.db_tip = DbTip{
        .id = block_id,
        .storage_long_hash = db_close.block().storage_long_hash,
        .value = value,
        .default_wallet = last_committed_block_.default_wallet,
        .network_config = last_committed_block_.network_config,
    };
    // These transactions intentionally never entered Scheduler::tx_cache_.
    // The ordinary commit path uses tx_hashes only to evict nominated mempool
    // entries; leave that eviction set empty while retaining the committed
    // hashes in ProposalTxs above.
    cache.tx_hashes.clear();
  }

  // A joint DB/event commit may already have succeeded in one subsystem when
  // the other reports failure. It is not safe to abandon or retry that block
  // in-process; fail-stop so startup recovery can reconcile durable state.
  SchedulerCommitOutcome commit_outcome;
  try {
    commit_outcome = co_await schedulercoord_commit_block_impl(block_id, value);
  } catch (const std::exception &e) {
    panic("genesis ceremony commit failed: {}", e.what());
  } catch (...) {
    panic("genesis ceremony commit failed with an unknown exception");
  }
  if (commit_outcome != SchedulerCommitOutcome::SchedulerCommitOK) {
    panic("genesis ceremony commit returned {}", commit_outcome);
  }
  GenesisCeremonyResult committed(
      GenesisCeremonyOutcome::GenesisCeremonyCommitted);
  // Assign the generated union arm's members directly and copy the nested
  // ParallelBatch vectors.  Moving the enclosing generated type can leave an
  // optimized GCC coroutine cleanup referring to a reused frame slot.
  committed.commit().db_tip = last_committed_block_;
  committed.commit().proposal_txs = committed_proposal_txs;
  co_return committed;
}

x23::Async<bool>
MempoolCoordinatorRpc::validate_batch_chunk(
    BlockID block_id, x23::span<const ProposedTransaction> txs,
    size_t executor_idx)
{
  ExecValidateBatchResult res =
      co_await exec_clients_[executor_idx].executorsched_validate_batch(
          block_id, txs);
  CHECK_EQ(res.size(), txs.size());
  CHECK(!std::ranges::contains(res, ExecResult::ExecResultUnrecoverable));
  // Validation succeeds if every entry in the executor response is a
  // deterministic outcome (OK, deterministic error, or root frame revert).
  // System errors are nondeterministic and cause validation to fail.
  co_return !(std::ranges::any_of(res, tx_failed));
}

x23::Async<MempoolCoordinatorRpc::PreparedValidationChunk>
MempoolCoordinatorRpc::prepare_validation_chunk(
    x23::span<const TransactionHash> tx_hashes,
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
           x23::span<const TransactionHash> tx_hashes,
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
  std::exception_ptr first_exception;
  for (x23::Async<void> &handle : lookup_handles) {
    try {
      co_await handle;
    } catch (...) {
      if (!first_exception) {
        first_exception = std::current_exception();
      }
    }
  }
  if (first_exception) {
    std::rethrow_exception(first_exception);
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
    BlockID block_id, x23::span<const TransactionHash> tx_hashes,
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

  x23::Vec<bool> results(large_chunks.size(), false);
  std::exception_ptr first_exception;
  for (size_t i = 0; i < handles.size(); ++i) {
    try {
      results[i] = co_await handles[i];
    } catch (...) {
      if (!first_exception) {
        first_exception = std::current_exception();
      }
    }
  }
  if (first_exception) {
    std::rethrow_exception(first_exception);
  }
  co_return std::ranges::all_of(results, std::identity{});
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

  x23::Vec<bool> results(chunks.size(), false);
  std::exception_ptr first_exception;
  for (size_t i = 0; i < handles.size(); ++i) {
    try {
      results[i] = co_await handles[i];
    } catch (...) {
      if (!first_exception) {
        first_exception = std::current_exception();
      }
    }
  }
  if (first_exception) {
    std::rethrow_exception(first_exception);
  }
  co_return std::ranges::all_of(results, std::identity{});
}

x23::Async<ExecResult>
MempoolCoordinatorRpc::open_validation_block(
    BlockID block_id, BlockExecutionContext block_execution,
    size_t executor_idx)
{
  co_return co_await exec_clients_[executor_idx]
      .executorsched_open_validation_block(block_id, block_execution);
}

x23::Async<SchedulerValidationResult>
MempoolCoordinatorRpc::schedulercoord_validate_block(BlockID parent_id,
                                                     Proposal proposal)
{
  if (!ready()) {
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationTooLateToValidate);
  }

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
  if (proposal.header.block_height <= 1) {
    // Heights 0 and 1 are established outside ordinary validation.
    validation_outcome = SchedulerValidationOutcome::SchedulerValidationInvalid;
    info("schedulercoord_validate ret");
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }
  while (true) {
    while (!(co_await increment_num_validations_in_progress())) {
      co_await wait_for_validation_gate_open();
    }

    if (proposal.header.block_height <
        last_committed_block_.value.proposal_header.block_height + 1) {
      // This block is too old to validate.
      co_await decrement_num_validations_in_progress();
      validation_outcome =
          SchedulerValidationOutcome::SchedulerValidationTooLateToValidate;
      info("schedulercoord_validate ret");
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationTooLateToValidate);
    }
    else if (proposal.header.block_height >
             last_committed_block_.value.proposal_header.block_height + 1) {
      // This block is ahead of the current block in the mempool. Thus, we need
      // to wait for the mempool to catch up before verifying this block.
      co_await decrement_num_validations_in_progress();
      // If we want to validate a block at height [x], we need to wait for the
      // block at height [x - 1] to commit.
      co_await wait_for_block_commit(proposal.header.block_height - 1);
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
  CHECK_EQ(proposal.header.block_height,
           last_committed_block_.value.proposal_header.block_height + 1);

  if (last_committed_block_.id != parent_id) {
    // Release the validation-gate increment taken at the top of this loop.
    // Without this, the increment leaks: [close_validation_gate()] then waits
    // forever for the count to reach zero, and every subsequent
    // [schedulercoord_commit_block()] wedges. (The bench harness maps a
    // timeout to success, so a wedged validator is silently masked in CI.)
    co_await decrement_num_validations_in_progress();
    validation_outcome =
        SchedulerValidationOutcome::SchedulerValidationUnknownParentBlock;
    info("schedulercoord_validate ret");
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationUnknownParentBlock);
  }
  if (proposal.header.network_hash != network_hash_) {
    co_await decrement_num_validations_in_progress();
    validation_outcome = SchedulerValidationOutcome::SchedulerValidationInvalid;
    info("schedulercoord_validate ret");
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }
  const TimePoint committed_timepoint =
      last_committed_block_.value.proposal_header.block_timepoint;
  // The next block must have a [block_timepoint] that is >= the last committed
  // block's timepoint.
  if (proposal.header.block_timepoint < committed_timepoint) {
    co_await decrement_num_validations_in_progress();
    validation_outcome = SchedulerValidationOutcome::SchedulerValidationInvalid;
    error("Rejecting block at height {}: timepoint {} predates the committed "
          "tip's {}",
          proposal.header.block_height, proposal.header.block_timepoint,
          committed_timepoint);
    co_return SchedulerValidationResult(
        SchedulerValidationOutcome::SchedulerValidationInvalid);
  }

  // TODO(jhumphri): Need to ensure that the node that proposed the block is
  // following the expected scheduling policy.
  x23::XdrVec<NondeterministicProposalData> tx_nominations;
  x23::XdrVec<TransactionHash> missing_tx_hashes;
  PendingBlock cache{};
  bool validated = true;
  SchedulerValidationResult result(
      SchedulerValidationOutcome::SchedulerValidationInvalid);
  SchedulerValidationOutcome dag_outcome =
      SchedulerValidationOutcome::SchedulerValidationInvalid;
  bool log_validation_return = true;
  BlockID validation_block_id{};
  std::atomic_bool validation_block_transferred{false};
  x23::Dag validation_dag("schedulercoord_validate", default_threadpool());

  x23::DagCleanup snapshot_cleanup = validation_dag.cleanup(
      "abandon validation snapshot", [&]() -> x23::Async<void> {
        if (validation_block_transferred.load(std::memory_order_acquire)) {
          co_return;
        }
        CHECK_EQ(co_await abandon_snapshot(validation_block_id),
                 DbSchedResult::DbSchedResultOK);
        co_return;
      });
  x23::DagCleanup event_cleanup = validation_dag.cleanup(
      "abandon validation event block", [&]() -> x23::Async<void> {
        if (validation_block_transferred.load(std::memory_order_acquire)) {
          co_return;
        }
        CHECK_EQ(co_await eventsched_abandon_block(validation_block_id),
                 RPCError::kRPCNone);
        co_return;
      });
  x23::Vec<x23::DagCleanup> executor_cleanups;
  executor_cleanups.reserve(exec_clients_.size());
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    executor_cleanups.push_back(
        validation_dag.cleanup("finish executor", [&, i]() -> x23::Async<void> {
          co_await finish_block(validation_block_id,
                                /*executor_idx=*/i);
          co_return;
        }));
  }

  x23::CoroLock set_result_lock;
  auto set_result =
      [&](SchedulerValidationOutcome outcome) -> x23::Async<void> {
    x23::CoroGuard cg;
    co_await cg(set_result_lock);

    dag_outcome = outcome;
    result = SchedulerValidationResult(outcome);
    co_return;
  };

  x23::CoroLock cancel_with_result_lock;
  std::atomic<bool> cancel_with_result_set{false};
  auto cancel_with_result = [&](SchedulerValidationOutcome outcome,
                                const char *node) -> x23::Async<void> {
    x23::CoroGuard cg;
    co_await cg(cancel_with_result_lock);

    if (!cancel_with_result_set.load(std::memory_order_acquire)) {
      co_await set_result(outcome);
      cancel_with_result_set.store(true, std::memory_order_relaxed);
    }
    validation_dag.fail(x23::DagFailureKind::Cancelled, node);
    co_return;
  };

  auto decode_node = validation_dag.async<void>(
      "decode nomination batches", [&]() -> x23::Async<void> {
        // We call [reserve] here to avoid racing with accesses to
        // [cache.tx_hashes] in [parallel_batch_to_t()] below.
        cache.txs.reserve(kMaxBlockSize);
        cache.tx_hashes.reserve(kMaxBlockSize);
        cache.result_tx_hashes.reserve(kMaxBlockSize);
        cache.result_tx_nominations.reserve(kMaxBlockSize);

        // The coordinator RPC runs with [kBulkRpcMaxMessageSize], so a peer
        // can claim far more transactions than a block holds.  Each claimed
        // hash costs an allocation and a [find_tx] lookup before anything
        // rejects it, and any that resolve go on to the executors.
        x23::Async<bool> tx_nondeterministic_data_handle =
            parallel_batch_to_t(proposal.transactions.nondeterministic_data,
                                tx_nominations, kMaxBlockSize);
        x23::Async<bool> tx_hash_handle =
            parallel_batch_to_t(proposal.transactions.hash, cache.tx_hashes,
                                kMaxBlockSize);

        std::exception_ptr first_exception;
        bool decoded_tx_nominations = false;
        bool decoded_tx_hashes = false;
        // Await both spawned decodes even if either one throws.
        try {
          decoded_tx_nominations = co_await tx_nondeterministic_data_handle;
        } catch (...) {
          first_exception = std::current_exception();
        }
        try {
          decoded_tx_hashes = co_await tx_hash_handle;
        } catch (...) {
          if (!first_exception) {
            first_exception = std::current_exception();
          }
        }
        if (first_exception) {
          std::rethrow_exception(first_exception);
        }
        if (!decoded_tx_nominations || !decoded_tx_hashes) {
          info("schedulercoord_validate rejected malformed nomination batch");
          log_validation_return = false;
          co_await cancel_with_result(
              SchedulerValidationOutcome::SchedulerValidationMalformed,
              "decode nomination batches");
          co_return;
        }

        if (cache.tx_hashes.size() != tx_nominations.size()) {
          log_validation_return = false;
          co_await cancel_with_result(
              SchedulerValidationOutcome::SchedulerValidationMalformed,
              "decode nomination batches");
          co_return;
        }

        cache.block_id = BlockIdGenerator::generate(BlockIdLane::kMempool);
        validation_block_id = cache.block_id;
        validation_tx_count = cache.tx_hashes.size();
        co_return;
      });

  auto transfer_data_node =
      validation_dag.async<void>("wait transfer data",
                                 validation_dag.after(decode_node),
                                 [&]() -> x23::Async<void> {
                                   co_await wait_for_transfer_data(
                                       proposal.header.block_height - 1);
                                   co_return;
                                 });
  auto auth_data_node =
      validation_dag.async<void>("wait auth data",
                                 validation_dag.after(decode_node),
                                 [&]() -> x23::Async<void> {
                                   co_await wait_for_auth_data(
                                       proposal.header.block_height - 1);
                                   co_return;
                                 });

  auto create_snapshot_node = validation_dag.async<void>(
      "create validation snapshot",
      validation_dag.after(transfer_data_node, auth_data_node),
      [&]() -> x23::Async<void> {
        DbSchedResult snapshot_result = co_await create_snapshot(
            /*prev_block_id=*/last_committed_block_.id, validation_block_id,
            BlockExecutionMode::kBlockExecutionModeValidation);
        if (snapshot_result == DbSchedResult::DbSchedResultOK) {
          snapshot_cleanup.arm();
          co_return;
        }
        if (snapshot_result == DbSchedResult::DbSchedResultInvalidParentId) {
          co_await cancel_with_result(
              SchedulerValidationOutcome::SchedulerValidationUnknownParentBlock,
              "create validation snapshot");
          co_return;
        }
        CHECK_EQ(snapshot_result, DbSchedResult::DbSchedResultError);
        co_await cancel_with_result(
            SchedulerValidationOutcome::SchedulerValidationDbFailed,
            "create validation snapshot");
        co_return;
      });

  auto open_event_node = validation_dag.async<void>(
      "open event block", validation_dag.after(create_snapshot_node),
      [&]() -> x23::Async<void> {
        CHECK_EQ(co_await eventsched_open_block(validation_block_id),
                 RPCError::kRPCNone);
        event_cleanup.arm();
        co_return;
      });

  // From the proposal's header, never recomputed locally: these are what the
  // proposer executed against, and deriving them here would split consensus.
  const BlockExecutionContext validation_block_execution{
      .block_height = proposal.header.block_height,
      .block_timepoint = proposal.header.block_timepoint,
  };

  x23::Vec<x23::DagNode<void>> open_validation_block_nodes;
  open_validation_block_nodes.reserve(exec_clients_.size());
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    open_validation_block_nodes.push_back(validation_dag.async<void>(
        "open executor validation block", validation_dag.after(open_event_node),
        [&, i]() -> x23::Async<void> {
          ExecResult result =
              co_await open_validation_block(validation_block_id,
                                             validation_block_execution,
                                             /*executor_idx=*/i);
          if (result != ExecResult::ExecResultOK) {
            co_await cancel_with_result(
                SchedulerValidationOutcome::SchedulerValidationExecutorFailed,
                "open executor validation block");
            co_return;
          }
          executor_cleanups[i].arm();
          co_return;
        }));
  }

  auto prepare_validate_node = validation_dag.async<void>(
      "prepare and validate chunks",
      merge_dag_deps(validation_dag.after(open_event_node),
                     dag_deps_from_nodes(open_validation_block_nodes)),
      [&]() -> x23::Async<void> {
        // [nomination] includes the tx hashes and the nondeterministic
        // proposal data. We use these tx hashes to look up the vetted txs in
        // [vetted_txs_]. Assuming we find all of the txs, we then combine them
        // with the nondeterministic proposal data to form a
        // [ProposedTransaction], which is what the executors expect.
        const std::vector<ChunkRange> validation_chunks =
            make_chunks_for_max_size(cache.tx_hashes.size(),
                                     kNumTxsPerNominationChunk);
        x23::Vec<x23::Async<PreparedAndValidatedChunk>> chunk_handles;
        chunk_handles.reserve(validation_chunks.size());

        for (const ChunkRange &chunk : validation_chunks) {
          x23::span<const TransactionHash> tx_hashes_span(
              cache.tx_hashes.begin() + chunk.start,
              cache.tx_hashes.begin() + chunk.end);
          x23::span<const NondeterministicProposalData> tx_nominations_span(
              tx_nominations.begin() + chunk.start,
              tx_nominations.begin() + chunk.end);
          chunk_handles.push_back(
              prepare_and_validate_chunk(validation_block_id, tx_hashes_span,
                                         tx_nominations_span));
        }

        x23::Vec<std::optional<PreparedAndValidatedChunk>> chunk_results(
            chunk_handles.size());
        std::exception_ptr first_exception;
        for (size_t i = 0; i < chunk_handles.size(); ++i) {
          try {
            chunk_results[i].emplace(co_await chunk_handles[i]);
          } catch (...) {
            if (!first_exception) {
              first_exception = std::current_exception();
            }
          }
        }
        if (first_exception) {
          std::rethrow_exception(first_exception);
        }
        for (std::optional<PreparedAndValidatedChunk> &settled :
             chunk_results) {
          if (!settled) {
            panic("validation result missing without a captured exception");
          }
          PreparedAndValidatedChunk &chunk_result = *settled;
          validated = chunk_result.validated && validated;
          missing_tx_hashes += chunk_result.missing_tx_hashes;
          cache.txs += chunk_result.txs;
        }
        co_return;
      });

  // A nomination is free to repeat a transaction hash: the batches still
  // decode and still have equal element counts, so nothing upstream rejects
  // one. [try_get_txset_hash()] reports the repeat rather than asserting on it.
  auto txset_hash_node = validation_dag.cpu<std::optional<SHA256Hash>>(
      "compute txset hash", validation_dag.after(decode_node),
      [&]() { return try_get_txset_hash(cache.tx_hashes); });

  auto check_validation_node = validation_dag.async<void>(
      "check validation chunks", validation_dag.after(prepare_validate_node),
      [&]() -> x23::Async<void> {
        if (!missing_tx_hashes.empty()) {
          dag_outcome =
              SchedulerValidationOutcome::SchedulerValidationMissingTxs;
          result = SchedulerValidationResult(
              SchedulerValidationOutcome::SchedulerValidationMissingTxs);
          result.missing_tx_hashes() = missing_tx_hashes;
          validation_dag.fail(x23::DagFailureKind::Cancelled,
                              "check validation chunks");
          co_return;
        }
        CHECK_EQ(cache.txs.size(), tx_nominations.size());

        if (!validated) {
          co_await cancel_with_result(
              SchedulerValidationOutcome::SchedulerValidationInvalid,
              "check validation chunks");
          co_return;
        }
        co_return;
      });

  x23::Vec<x23::DagNode<void>> finish_executor_nodes;
  finish_executor_nodes.reserve(exec_clients_.size());
  for (size_t i = 0; i < exec_clients_.size(); i++) {
    finish_executor_nodes.push_back(
        validation_dag.async<void>("finish executor",
                                   validation_dag.after(check_validation_node),
                                   [&, i]() -> x23::Async<void> {
                                     co_await finish_block(validation_block_id,
                                                           /*executor_idx=*/i);
                                     executor_cleanups[i].disarm();
                                     co_return;
                                   }));
  }
  auto close_snapshot_node = validation_dag.async<DbBlockResult>(
      "close validation snapshot", validation_dag.after(check_validation_node),
      [&]() -> x23::Async<DbBlockResult> {
        co_return co_await close_snapshot(validation_block_id);
      });
  auto proposal_txs_hash_node = validation_dag.cpu<ProposalTxsHash>(
      "compute proposal txs hash", validation_dag.after(check_validation_node),
      [&proposal] {
        return proposal_txs_compatibility_hash(proposal.transactions);
      });
  auto close_event_node = validation_dag.async<EventBlockResult>(
      "close event block", validation_dag.after(check_validation_node),
      [&]() -> x23::Async<EventBlockResult> {
        co_return co_await eventsched_close_block(validation_block_id,
                                                  scheduler_->current_block() +
                                                      1);
      });
  x23::DagDeps close_validation_deps =
      merge_dag_deps(validation_dag.after(close_snapshot_node, close_event_node,
                                          txset_hash_node),
                     dag_deps_from_nodes(finish_executor_nodes));

  auto check_closed_node = validation_dag.async<void>(
      "check closed validation block", std::move(close_validation_deps),
      [&]() -> x23::Async<void> {
        {
          EventBlockResult event_result = close_event_node.get();
          CHECK_EQ(event_result.error(), RPCError::kRPCNone);
          cache.db_tip.value.proposal_header.events_hash =
              event_result.root_hash();
        }
        {
          DbBlockResult db_result = close_snapshot_node.get();
          // A forged RCQ observation can pass executor validation and be
          // rejected only by hog_db at [dbsched_close_snapshot()], so an
          // invalid nominated block can reach this path.
          if (db_result.result() != DbSchedResult::DbSchedResultOK) {
            co_await cancel_with_result(
                SchedulerValidationOutcome::SchedulerValidationInvalid,
                "check closed validation block");
            co_return;
          }
          cache.db_tip.value.proposal_header.storage_hash =
              db_result.block().storage_hash;
          cache.db_tip.storage_long_hash = db_result.block().storage_long_hash;
          cache.db_tip.value.proposal_header.config_hash =
              compute_config_hash(db_result.block().config_hash_preimage);
        }

        // A duplicated transaction hash leaves the txset hash uncomputable,
        // so the nomination cannot match [proposal.header.txs_hash] and is
        // rejected here alongside every other header mismatch.
        const std::optional<SHA256Hash> &txset_hash = txset_hash_node.get();
        if (!txset_hash.has_value() ||
            *txset_hash != proposal.header.txs_hash ||
            cache.db_tip.value.proposal_header.storage_hash !=
                proposal.header.storage_hash ||
            cache.db_tip.value.proposal_header.config_hash !=
                proposal.header.config_hash ||
            cache.db_tip.value.proposal_header.events_hash !=
                proposal.header.events_hash) {
          // The event block and DB snapshot were closed above, but a hash
          // mismatch means this block is never added to
          // [validated_block_cache_], so [schedulercoord_commit_block] will
          // never abandon it.
          //
          // [snapshot_cleanup] and [event_cleanup] abandon both to ensure that
          // we do not leak the event block context and orphan its events file.
          co_await cancel_with_result(
              SchedulerValidationOutcome::SchedulerValidationInvalid,
              "check closed validation block");
          co_return;
        }
        co_return;
      });

  validation_dag.async<void>(
      "build validation result",
      validation_dag.after(check_closed_node, proposal_txs_hash_node),
      [&]() -> x23::Async<void> {
        ConsensusValue value{.protocol = 0,
                             .ext = {},
                             .prev_hash = sha256(last_committed_block_.value),
                             .proposal_header = proposal.header,
                             .proposal_txs_hash = proposal_txs_hash_node.get(),
                             .upgrades = {}};
        // default_wallet and network_config come from the committed parent
        // tip (see the nomination-path comment).
        cache.db_tip =
            DbTip{.id = validation_block_id,
                  .storage_long_hash = cache.db_tip.storage_long_hash,
                  .value = std::move(value),
                  .default_wallet = last_committed_block_.default_wallet,
                  .network_config = last_committed_block_.network_config};
        co_await set_result(SchedulerValidationOutcome::SchedulerValidationOK);
        result.block().block_id = cache.db_tip.id;
        result.block().signed_transactions = {};
        co_await t_to_parallel_batch(cache.txs,
                                     result.block().signed_transactions);

        const BlockID block_id = validation_block_id;
        {
          x23::CoroGuard cache_cg;
          co_await cache_cg(validated_block_cache_lock_);
          CHECK(!validated_block_cache_.contains(block_id));
          validated_block_cache_[block_id] = std::move(cache);
          validation_block_transferred.store(true, std::memory_order_release);
        }
        snapshot_cleanup.disarm();
        event_cleanup.disarm();
        co_return;
      });

  co_await validation_dag.run();
  co_await decrement_num_validations_in_progress();
  validation_outcome = dag_outcome;
  if (!validation_dag.ok() && validation_dag.failure().exception) {
    std::rethrow_exception(validation_dag.failure().exception);
  }
  if (const char *dot_dir = std::getenv("MEMPOOL_DAG_DOT_DIR");
      dot_dir != nullptr && *dot_dir != '\0') {
    static std::atomic<size_t> dot_counter{0};
    const size_t dot_idx = dot_counter.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path dot_path =
        std::filesystem::path(dot_dir) /
        ("mempool_validate_" + std::to_string(dot_idx) + ".dot");
    std::ofstream graph_dot(dot_path, std::ios::out | std::ios::trunc);
    graph_dot << validation_dag.dot();
  }
  if (log_validation_return) {
    info("schedulercoord_validate ret");
  }
  co_return result;
}

// TODO(jhumphri): We could livelock in this method if we continue to receive
// blocks to validate. We do not pause validations once
// [schedulercoord_commit_block()] starts. If this becomes an issue (such as due
// to a malicious node that keeps sending blocks to validate), we will need to
// stop validations once a commit starts.
x23::Async<SchedulerCommitOutcome>
MempoolCoordinatorRpc::schedulercoord_commit_block(
    BlockID block_id, ConsensusValue expected_value)
{
  if (!ready()) {
    co_return SchedulerCommitOutcome::SchedulerCommitBlockNotFound;
  }
  if (expected_value.proposal_header.block_height <= 1) {
    // Heights 0 and 1 are committed by genesis initialization and the genesis
    // ceremony, not through the ordinary coordinator commit RPC.
    co_return SchedulerCommitOutcome::SchedulerCommitInvalidConsensusValue;
  }
  co_return co_await schedulercoord_commit_block_impl(block_id,
                                                      std::move(
                                                          expected_value));
}

x23::Async<SchedulerCommitOutcome>
MempoolCoordinatorRpc::schedulercoord_commit_block_impl(
    BlockID block_id, ConsensusValue expected_value)
{
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
    case CommitMetricsOutcome::kConcurrent:
      MempoolMetrics::instance().commit_concurrent_count.increment();
      break;
    }
    MempoolMetrics::instance().commit_reenqueued_tx_count.increment(
        reenqueue_size);
    MempoolMetrics::instance().commit_removed_tx_count.increment(
        remove_from_scheduler_size);
  };

  if (!(co_await close_validation_gate())) {
    // This is a concurrent call to [close_validation_gate()], so there must be
    // another RPC that is already trying to commit the block.
    emit_commit_stats(CommitMetricsOutcome::kConcurrent,
                      /*reenqueue_size=*/0,
                      /*remove_from_scheduler_size=*/0);
    co_return SchedulerCommitOutcome::SchedulerConcurrentCommit;
  }

  x23::Defer reopen_validation_gate([this]() noexcept {
    try {
      open_validation_gate();
    } catch (...) {
      std::terminate();
    }
  });

  struct CommitPlan {
    const PendingBlock *committed_cache{nullptr};
    bool committed_nominated{false};
    x23::Vec<BlockID> abandon_block_ids;
  };

  CommitPlan commit_plan;
  SchedulerCommitOutcome commit_outcome =
      SchedulerCommitOutcome::SchedulerCommitBlockNotFound;
  bool invalid_consensus_value = false;
  size_t reenqueue_size = 0;
  size_t remove_from_scheduler_size = 0;

  auto nominated_guard = std::make_shared<x23::CoroGuard>();
  auto validated_guard = std::make_shared<x23::CoroGuard>();
  auto commit_guard = std::make_shared<x23::CoroGuard>();
  auto hog_age_guard = std::make_shared<x23::CoroGuard>();
  x23::Dag commit_dag("schedulercoord_commit_block", default_threadpool());
  x23::DagCleanup fatal_commit_cleanup = commit_dag.cleanup(
      "fail internal commit error", [&]() -> x23::Async<void> {
        const x23::DagFailure failure = commit_dag.failure();
        // [failure.exception] is an [std::exception_ptr] which is an opaque
        // type, so we throw it and then catch it below so that we can log
        // details about the exception.
        try {
          if (failure.exception) {
            std::rethrow_exception(failure.exception);
          }
          error("internal block {} commit failure in DAG node '{}'", block_id,
                failure.node);
        } catch (const std::exception &e) {
          error("internal block {} commit failure in DAG node '{}': {}",
                block_id, failure.node, e.what());
        } catch (...) {
          error("internal block {} commit failure in DAG node '{}': unknown "
                "exception",
                block_id, failure.node);
        }
        CHECK(false);
        co_return;
      });
  fatal_commit_cleanup.arm();

  auto lock_nominated_node = commit_dag.async<void>(
      "lock nominated block cache", [&, nominated_guard]() -> x23::Async<void> {
        co_await (*nominated_guard)(nominated_block_cache_lock_);
        co_return;
      });

  auto lock_validated_node = commit_dag.async<void>(
      "lock validated block cache", [&, validated_guard]() -> x23::Async<void> {
        co_await (*validated_guard)(validated_block_cache_lock_);
        co_return;
      });

  auto check_block_node = commit_dag.async<void>(
      "check block exists",
      commit_dag.after(lock_nominated_node, lock_validated_node),
      [&]() -> x23::Async<void> {
        if ((!nominated_block_cache_.block_id ||
             nominated_block_cache_.block_id != block_id) &&
            !validated_block_cache_.contains(block_id)) {
          emit_commit_stats(CommitMetricsOutcome::kBlockNotFound,
                            /*reenqueue_size=*/0,
                            /*remove_from_scheduler_size=*/0);
          // A block with ID [block_id] was neither nominated nor validated, so
          // it cannot be committed.
          info("schedulercoord_commit_block ret");
          // This is a failure caused by an external user, not an internal
          // commit failure.
          fatal_commit_cleanup.disarm();
          commit_dag.fail(x23::DagFailureKind::Cancelled, "check block exists");
        }
        co_return;
      });

  auto lock_commit_node =
      commit_dag.async<void>("lock commit", commit_dag.after(check_block_node),
                             [&, commit_guard]() -> x23::Async<void> {
                               co_await (*commit_guard)(commit_lock_);
                               co_return;
                             });

  // We use [scheduler_->pending_hog_age_update_lock_] to synchronize hog age
  // updates with [MempoolDbRpc], which calls [scheduler_->process_transfers()].
  // We want to ensure that on a block commit, we are able to apply the pending
  // hog ages in the scheduler *before* the hog transfer deltas are received
  // from hog_db. Otherwise, if the transfer deltas are applied first, it is
  // possible that they will transfer hogs between accounts, and then when we
  // apply the pending hog ages after, we may try to move hogs between scheduler
  // groups for an account that no longer has any hogs.
  auto lock_hog_age_node =
      commit_dag.async<void>("lock pending hog age update",
                             commit_dag.after(check_block_node),
                             [&, hog_age_guard]() -> x23::Async<void> {
                               co_await (*hog_age_guard)(
                                   scheduler_->pending_hog_age_update_lock_);
                               co_return;
                             });

  auto prepare_node = commit_dag.async<void>(
      "prepare commit plan",
      commit_dag.after(check_block_node, lock_commit_node),
      [&]() -> x23::Async<void> {
        commit_plan.abandon_block_ids.clear();
        if (nominated_block_cache_.block_id &&
            nominated_block_cache_.block_id == block_id) {
          // We are committing the block that we nominated.
          commit_plan.committed_cache = &nominated_block_cache_;
          commit_plan.committed_nominated = true;
        }
        else {
          // We are committing a block that we validated.
          auto it = validated_block_cache_.find(block_id);
          CHECK(it != validated_block_cache_.end());
          commit_plan.committed_cache = &it->second;
          commit_plan.committed_nominated = false;
          if (nominated_block_cache_.block_id) {
            // We nominated a block, so we need to abandon the block.
            commit_plan.abandon_block_ids.push_back(
                nominated_block_cache_.block_id);
          }
        }

        for (const auto &[_, cache] : validated_block_cache_) {
          if (cache.block_id != block_id) {
            commit_plan.abandon_block_ids.push_back(cache.block_id);
          }
        }
        CHECK_PTR(commit_plan.committed_cache);
        if (commit_plan.committed_cache->db_tip.value != expected_value) {
          info("schedulercoord_commit_block ret");
          invalid_consensus_value = true;
          commit_outcome =
              SchedulerCommitOutcome::SchedulerCommitInvalidConsensusValue;
          co_return;
        }
        co_return;
      });

  auto commit_block_node = commit_dag.async<JointCommitResult>(
      "commit block", commit_dag.after(prepare_node, lock_hog_age_node),
      [&]() -> x23::Async<JointCommitResult> {
        if (invalid_consensus_value) {
          co_return JointCommitResult{};
        }
        CHECK_PTR(commit_plan.committed_cache);
        co_return co_await commit_block(*commit_plan.committed_cache);
      });

  auto abandon_snapshots_node = commit_dag.async<void>(
      "abandon snapshots", commit_dag.after(prepare_node),
      [&]() -> x23::Async<void> {
        if (invalid_consensus_value) {
          co_return;
        }
        x23::Vec<DbSchedResult> results(commit_plan.abandon_block_ids.size(),
                                        DbSchedResult::DbSchedResultError);
        x23::Vec<x23::Async<DbSchedResult>> snapshot_handles;
        snapshot_handles.reserve(commit_plan.abandon_block_ids.size());
        for (const BlockID id : commit_plan.abandon_block_ids) {
          snapshot_handles.push_back(abandon_snapshot(id));
        }
        std::exception_ptr first_exception;
        for (size_t i = 0; i < snapshot_handles.size(); ++i) {
          try {
            results[i] = co_await snapshot_handles[i];
          } catch (...) {
            if (!first_exception) {
              first_exception = std::current_exception();
            }
          }
        }
        if (first_exception) {
          std::rethrow_exception(first_exception);
        }
        for (const DbSchedResult result : results) {
          CHECK_EQ(result, DbSchedResult::DbSchedResultOK);
        }
        co_return;
      });

  auto abandon_events_node = commit_dag.async<void>(
      "abandon event blocks", commit_dag.after(commit_block_node),
      [&]() -> x23::Async<void> {
        if (invalid_consensus_value) {
          co_return;
        }
        x23::Vec<RPCError> results(commit_plan.abandon_block_ids.size(),
                                   RPCError::kRPCUnrecoverable);
        x23::Vec<x23::Async<RPCError>> event_handles;
        event_handles.reserve(commit_plan.abandon_block_ids.size());
        for (const BlockID id : commit_plan.abandon_block_ids) {
          event_handles.push_back(eventsched_abandon_block(id));
        }
        std::exception_ptr first_exception;
        for (size_t i = 0; i < event_handles.size(); ++i) {
          try {
            results[i] = co_await event_handles[i];
          } catch (...) {
            if (!first_exception) {
              first_exception = std::current_exception();
            }
          }
        }
        if (first_exception) {
          std::rethrow_exception(first_exception);
        }
        for (const RPCError result : results) {
          CHECK_EQ(result, RPCError::kRPCNone);
        }
        co_return;
      });

  // Scheduler updates only need the commit plan and hog-age lock. Run them in
  // parallel with external commit/abandon RPCs so committed txs stop occupying
  // scheduler capacity while those RPCs finish.
  auto scheduler_update_node = commit_dag.async<void>(
      "update scheduler", commit_dag.after(prepare_node, lock_hog_age_node),
      [&]() -> x23::Async<void> {
        if (invalid_consensus_value) {
          co_return;
        }

        // We need to do two things:
        // 1. If the nominated block is not the committed block, we need to
        // return the nominated block's txs to the scheduler.
        //
        // 2. Next, we need to remove the committed block's txs from the
        // scheduler. Note that some of these txs may overlap with the txs in
        // the nominated block, so we can optimize this by only returning to the
        // scheduler the txs in the nominated block that are not in the
        // committed block. Then we can remove all of the committed block's txs
        // from the scheduler.
        //
        // Importantly, this logic must only look at txs that actually made it
        // into the nominated block. [nominated_block_cache_.tx_hashes] includes
        // every tx we attempted during scheduling, including txs that later
        // failed proposal and were added to [failed_tx_hashes].
        static std::unordered_set<TransactionHash, SipHash24Hasher>
            return_to_runqueue;
        return_to_runqueue.clear();

        CHECK_PTR(commit_plan.committed_cache);
        const x23::XdrVec<TransactionHash> *remove_from_scheduler = nullptr;
        if (commit_plan.committed_nominated) {
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
            // scheduled again. [nominated_block_cache_.tx_hashes] also
            // includes the failed txs (in
            // [nominated_block_cache_.failed_tx_hashes]).
            return_to_runqueue.insert(nominated_block_cache_.tx_hashes.begin(),
                                      nominated_block_cache_.tx_hashes.end());
          }
          remove_from_scheduler = &commit_plan.committed_cache->tx_hashes;

          // Any of the txs in the committed (validated) block that were also in
          // the nominated block should not be returned to the runqueue.
          for (const TransactionHash &hash : *remove_from_scheduler) {
            if (return_to_runqueue.empty()) {
              break;
            }
            return_to_runqueue.erase(hash);
          }
        }

        scheduler_->reenqueue_txs(return_to_runqueue.cbegin(),
                                  return_to_runqueue.cend());
        reenqueue_size = return_to_runqueue.size();
        CHECK_PTR(remove_from_scheduler);
        remove_from_scheduler_size = remove_from_scheduler->size();
        if (commit_plan.committed_nominated) {
          // We committed the block we nominated.
          for (const TransactionHash &hash :
               nominated_block_cache_.result_tx_hashes) {
            co_await scheduler_->record_transaction_outcome(
                hash, /*status=*/
                SchedulerTransactionStatus::SchedulerTransactionSucceeded,
                /*error_source=*/
                SchedulerTransactionErrorSource::SchedulerTransactionErrorNone,
                /*error_code=*/0);
          }
          CHECK_EQ(nominated_block_cache_.failed_tx_hashes.size(),
                   nominated_block_cache_.failed_tx_results.size());
          for (size_t i = 0; i < nominated_block_cache_.failed_tx_hashes.size();
               i++) {
            const ExecResult result =
                nominated_block_cache_.failed_tx_results[i];
            co_await scheduler_->record_transaction_outcome(
                /*tx_hash=*/nominated_block_cache_.failed_tx_hashes[i],
                /*status=*/transaction_status_for_exec(result),
                /*error_source=*/
                SchedulerTransactionErrorSource::
                    SchedulerTransactionErrorExecutor,
                /*error_code=*/static_cast<int32_t>(result));
          }
        }
        else {
          // We committed a block we validated.
          for (const TransactionHash &hash : *remove_from_scheduler) {
            co_await scheduler_->record_transaction_outcome(
                hash, /*status=*/
                SchedulerTransactionStatus::SchedulerTransactionSucceeded,
                /*error_source=*/
                SchedulerTransactionErrorSource::SchedulerTransactionErrorNone,
                /*error_code=*/0);
          }
        }
        co_await scheduler_->remove_from_scheduler(
            x23::span<const TransactionHash>(*remove_from_scheduler));
        if (commit_plan.committed_nominated) {
          x23::CoroGuard cg;
          co_await cg(scheduler_->global_lock_);
          co_await scheduler_->commit_pending_nomination(block_id);
        }
        else {
          if (nominated_block_cache_.block_id) {
            x23::CoroGuard cg;
            co_await cg(scheduler_->global_lock_);
            co_await scheduler_->abandon_pending_nomination(
                nominated_block_cache_.block_id);
          }
          // [apply_committed_block_hog_ages] acquires
          // [scheduler_->global_lock_] for us, so we do not need to do it here.
          co_await scheduler_->apply_committed_block_hog_ages(
              commit_plan.committed_cache->txs,
              commit_plan.committed_cache->db_tip.value.proposal_header
                  .block_height);
        }
        co_return;
      });

  // HogDb commit publishes transfer deltas, and MempoolDbRpc writes average hog
  // ages to DB after processing those deltas. Keep this lock held until the
  // scheduler has applied pending hog ages and DB commit is complete, matching
  // the synchronization the pre-DAG code relied on.
  auto release_hog_age_node =
      commit_dag.async<void>("release pending hog age update",
                             commit_dag.after(scheduler_update_node,
                                              commit_block_node),
                             [hog_age_guard]() -> x23::Async<void> {
                               (*hog_age_guard).reset();
                               co_return;
                             });

  commit_dag.async<void>(
      "finalize commit",
      commit_dag.after(release_hog_age_node, abandon_snapshots_node,
                       abandon_events_node),
      [&]() -> x23::Async<void> {
        if (invalid_consensus_value) {
          co_return;
        }
        const JointCommitResult &joint_commit = commit_block_node.get();
        CHECK_EQ(joint_commit.db.result(), DbSchedResult::DbSchedResultOK);
        CHECK_EQ(joint_commit.event_commit, RPCError::kRPCNone);
        // Do not publish the tip until both subsystems have committed it. Copy
        // before clearing the caches because [committed_cache] may point into
        // [validated_block_cache_].
        CHECK_PTR(commit_plan.committed_cache);
        last_committed_block_ = commit_plan.committed_cache->db_tip;
        // Be careful to not do [nominated_block_cache_ = {}]. We pre-allocate
        // elements in the vectors in [nominated_block_cache_], so if we clear
        // those vectors, we will experience segfaults because we will later
        // attempt to read/write indices that do not exist.
        nominated_block_cache_.block_id = 0;
        validated_block_cache_.clear();

        co_await scheduler_->increment_current_block();
        co_await Health::instance().set_current_block_height(
            last_committed_block_.value.proposal_header.block_height);
        MempoolMetrics::instance().current_block_height.record(
            last_committed_block_.value.proposal_header.block_height);
        MempoolMetrics::instance().scheduler_pending_tx_count.record(
            scheduler_->size());
        MempoolMetrics::instance().scheduler_tx_cache_size.record(
            scheduler_->tx_cache_size());
        Registration::registration().publish_event_canonical_tip(
            EventCanonicalTip{.block_id = last_committed_block_.id,
                              .consensus_value = last_committed_block_.value});
        emit_commit_stats(CommitMetricsOutcome::kOk, reenqueue_size,
                          remove_from_scheduler_size);
        info("schedulercoord_commit_block ret");
        last_commit_ = std::chrono::steady_clock::now();
        commit_outcome = SchedulerCommitOutcome::SchedulerCommitOK;
        fatal_commit_cleanup.disarm();
        co_return;
      });

  co_await commit_dag.run();
  if (!commit_dag.ok() && commit_dag.failure().exception) {
    // An internal failure should have triggered [fatal_commit_cleanup]. We have
    // this extra CHECK here just in case there is a bug in our logic above.
    CHECK(false);
  }
  if (const char *dot_dir = std::getenv("MEMPOOL_DAG_DOT_DIR");
      dot_dir != nullptr && *dot_dir != '\0') {
    static std::atomic<size_t> dot_counter{0};
    const size_t dot_idx = dot_counter.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path dot_path =
        std::filesystem::path(dot_dir) /
        ("mempool_commit_" + std::to_string(dot_idx) + ".dot");
    std::ofstream graph_dot(dot_path, std::ios::out | std::ios::trunc);
    graph_dot << commit_dag.dot();
  }

  co_return commit_outcome;
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
  // Count this callback among the coordinator's pending coroutines so that
  // ~MempoolCoordinatorRpc waits for it to finish before the member
  // destructors tear down the state it touches -- in particular
  // [auth_data_height_], which we broadcast on at the end of this function.
  //
  // This callback is invoked from the [MempoolAuthRpc] handler, whose RPC
  // stream lifetime does not reliably cover this coroutine's tail: the tail
  // (the [auth_data_height_] broadcast) runs on a thread-pool worker after the
  // handler has already been resumed/failed, so draining [mempool_auth_server_]
  // during a failed construction is not enough on its own -- the broadcast can
  // still race the coordinator's destructor. Increment synchronously, before
  // the first co_await, so the count is raised while the auth server's drain is
  // still holding the handler; decrement via a scope guard so the count is
  // released even if this coroutine's frame is destroyed early (cancellation).
  pending_detached_coroutines_.fetch_add(1, std::memory_order_acquire);
  x23::Defer release_pending([this] {
    pending_detached_coroutines_.fetch_sub(1, std::memory_order_release);
  });

  CHECK(initialized_.load(std::memory_order_acquire));
  const BlockHeight current_height = auth_data_height_.load();
  if (height <= current_height) {
    // An auth server that subscribes after the database has committed its
    // current tip sends an empty synchronization callback for that tip.  A
    // restarted mempool may already have initialized its watermark from the
    // same database tip, so this notification is intentionally idempotent.
    warn("Ignoring stale auth-data update for height {} at current height {}",
         height, current_height);
    co_return;
  }
  CHECK_EQ(height, current_height + 1);

  x23::CoroGuard cg;
  co_await cg(commit_lock_);

  std::erase_if(submission_auth_,
                [](const std::weak_ptr<SubmissionAuth> &entry) {
                  return entry.expired();
                });
  for (const std::weak_ptr<SubmissionAuth> &entry : submission_auth_) {
    if (std::shared_ptr<SubmissionAuth> pending = entry.lock()) {
      for (const KeyChange &change : key_changes) {
        if (pending->accounts.contains(change.address)) {
          pending->dirty.insert(change.address);
        }
      }
    }
  }

  static TransactionHashBatch tx_hashes;
  static SignedTransactionBatch txs;
  tx_hashes.clear();
  txs.clear();
  for (const KeyChange &change : key_changes) {
    // Get all txs for [change.address] and re-verify them. Remove any that fail
    // verification.
    static TransactionHashBatch change_tx_hashes;
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
  CHECK_EQ(auth_ret.size(), tx_hashes.size());
  x23::Vec<TransactionHash> to_remove;
  for (size_t i = 0; i < auth_ret.size(); i++) {
    const AuthErrorCode &code = auth_ret[i];
    if (code != AuthErrorCode::kAuthSuccess) {
      co_await scheduler_->record_transaction_outcome(
          tx_hashes[i],
          /*status=*/transaction_status_for_auth(code),
          /*error_source=*/
          SchedulerTransactionErrorSource::SchedulerTransactionErrorAuth,
          /*error_code=*/static_cast<int32_t>(code));
      to_remove.push_back(tx_hashes[i]);
    }
  }
  co_await scheduler_->remove_from_scheduler(to_remove);
  auth_data_height_.store(height);
  auth_data_height_.broadcast(&default_threadpool());
}

} // namespace hogd
