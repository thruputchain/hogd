#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "exec_result.hh"
#include "exec_sched.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/interface/dispatch_pool.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "view.hh"
#include "x23/async.h"

namespace hogd {

// Context for executing transactions in a single block.
// Maintains the block's snapshot cache and any other cached data.
class ExecutorContext {
public:
  ExecutorContext(ExecutionMode mode, BlockID id, DatabaseProxy &db_proxy,
                  std::unique_ptr<StorageSnapshotCache> snapshot_cache,
                  EventServerProxy &event_server_proxy,
                  DispatchPool &worker_pool)
    : mode_(mode), block_id_(id), db_proxy_(db_proxy),
      event_server_proxy_(event_server_proxy),
      snapshot_cache_(std::move(snapshot_cache)), worker_pool_(worker_pool)
  {}

  // Disable copy and move
  ExecutorContext(ExecutorContext const &) = delete;
  ExecutorContext &operator=(ExecutorContext const &) = delete;
  ExecutorContext(ExecutorContext &&) = delete;
  ExecutorContext &operator=(ExecutorContext &&) = delete;

  ExecutionMode mode() const { return mode_; }

  x23::Async<ExecProposeBatchResult>
  propose_transaction_batch(TransactionBatch const &txs,
                            TransactionID first_tx_id);
  x23::Async<ExecValidateBatchResult>
  validate_transaction_batch(ProposedTransactionBatch const &proposed_txs,
                             TransactionID first_tx_id);

  x23::Async<ExecProposeResult> propose_transaction(Transaction const &tx,
                                                    TransactionID tx_id);
  x23::Async<ExecResult>
  validate_transaction(ProposedTransaction const &proposed_tx,
                       TransactionID tx_id);
  
  x23::Async<ExpectedViewResult>
  view_transaction(TransactionViewQuery const &query);

  std::unique_ptr<StorageSnapshotCache> extract_snapshot_cache()
  {
    // NOLINTNEXTLINE(readability-ambiguous-smartptr-reset-call)
    snapshot_cache_->reset();
    return std::move(snapshot_cache_);
  }

  // Block until all event calls have returned.
  x23::Async<std::optional<SystemErrorCode>> await_event_return();

private:
  PerTransactionDatabaseProxy make_per_tx_db_proxy(TransactionID tx_id);
  PerTransactionEventServerProxy
  make_per_tx_event_server_proxy(TransactionID tx_id);

  ExecProposeResult run_propose_transaction(Transaction const &tx,
                                            TransactionID tx_id);
  ExecResult run_validate_transaction(ProposedTransaction const &tx,
                                      TransactionID tx_id);

  ExpectedViewResult run_view_transaction(TransactionViewQuery const &query);

  void log_event_return(SystemStatus<void> error);

  const ExecutionMode mode_;
  const BlockID block_id_;
  DatabaseProxy &db_proxy_;
  EventServerProxy &event_server_proxy_;
  std::unique_ptr<StorageSnapshotCache> snapshot_cache_;
  DispatchPool &worker_pool_;

  std::mutex event_accumulate_mutex_;
  std::optional<SystemErrorCode> event_error_ = std::nullopt;
  std::atomic<uint32_t> pending_event_calls_ = 0;
};

} // namespace hogd
