#pragma once

#include <atomic>
#include <concepts>

#include "exec_coord.hh"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

class ExecutorContext {
public:
  ExecutorContext(ExecutionMode mode, BlockID id, DatabaseProxy &db_proxy,
                  WorkerPool &main_worker_pool)
    : mode_(mode), block_id_(id), db_proxy_(db_proxy),
      snapshot_cache_(id, db_proxy), main_worker_pool_(main_worker_pool)
  {}

  // Disable copy and move
  ExecutorContext(ExecutorContext const &) = delete;
  ExecutorContext &operator=(ExecutorContext const &) = delete;
  ExecutorContext(ExecutorContext &&) = delete;
  ExecutorContext &operator=(ExecutorContext &&) = delete;

  ExecutionMode mode() const { return mode_; }

  x23::Async<ExecProposeResult>
  propose_transaction(SignedTransaction const &signed_tx, TransactionID tx_id);
  x23::Async<ExecResult>
  validate_transaction(ProposedTransaction const &proposed_tx, TransactionID tx_id);

private:
  PerTransactionDatabaseProxy make_per_tx_db_proxy(TransactionID tx_id);

  const ExecutionMode mode_;
  const BlockID block_id_;
  DatabaseProxy &db_proxy_;
  StorageSnapshotCache snapshot_cache_;
  WorkerPool &main_worker_pool_;
};

} // namespace hogd
