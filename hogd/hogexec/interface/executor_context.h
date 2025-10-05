#pragma once

#include "exec_sched.hh"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "rpc_types.hh"
#include "transaction.hh"

namespace hogd {

// Context for executing transactions in a single block.
// Maintains the block's snapshot cache and any other cached data.
class ExecutorContext {
public:
  ExecutorContext(ExecutionMode mode, BlockID id, DatabaseProxy &db_proxy)
    : mode_(mode), block_id_(id), db_proxy_(db_proxy),
      snapshot_cache_(id, db_proxy)
  {}

  // Disable copy and move
  ExecutorContext(ExecutorContext const &) = delete;
  ExecutorContext &operator=(ExecutorContext const &) = delete;
  ExecutorContext(ExecutorContext &&) = delete;
  ExecutorContext &operator=(ExecutorContext &&) = delete;

  ExecutionMode mode() const { return mode_; }

  ExecProposeResult propose_transaction(SignedTransaction const &signed_tx,
                                        TransactionID tx_id);
  ExecResult validate_transaction(ProposedTransaction const &proposed_tx,
                                  TransactionID tx_id);

private:
  PerTransactionDatabaseProxy make_per_tx_db_proxy(TransactionID tx_id);

  const ExecutionMode mode_;
  const BlockID block_id_;
  DatabaseProxy &db_proxy_;
  StorageSnapshotCache snapshot_cache_;
};

} // namespace hogd
