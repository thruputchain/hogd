#pragma once

#include "exec_sched.hh"
#include "hogd/hogexec/interface/dispatch_pool.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"
#include <memory>
#include <utility>

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
  propose_transaction_batch(SignedTransactionBatch const &signed_txs,
                            TransactionID first_tx_id);
  x23::Async<ExecValidateBatchResult>
  validate_transaction_batch(ProposedTransactionBatch const &proposed_txs,
                             TransactionID first_tx_id);

  x23::Async<ExecProposeResult>
  propose_transaction(SignedTransaction const &signed_tx, TransactionID tx_id);
  x23::Async<ExecResult>
  validate_transaction(ProposedTransaction const &proposed_tx,
                       TransactionID tx_id);
  std::unique_ptr<StorageSnapshotCache> extract_snapshot_cache()
  {
    snapshot_cache_->reset();
    return std::move(snapshot_cache_);
  }

private:
  PerTransactionDatabaseProxy make_per_tx_db_proxy(TransactionID tx_id);
  PerTransactionEventServerProxy
  make_per_tx_event_server_proxy(TransactionID tx_id);

  const ExecutionMode mode_;
  const BlockID block_id_;
  DatabaseProxy &db_proxy_;
  EventServerProxy &event_server_proxy_;
  std::unique_ptr<StorageSnapshotCache> snapshot_cache_;
  DispatchPool &worker_pool_;

  ExecProposeResult run_propose_transaction(SignedTransaction const &tx,
                                            TransactionID tx_id);
  ExecResult run_validate_transaction(ProposedTransaction const &tx,
                                      TransactionID tx_id);
};

} // namespace hogd
