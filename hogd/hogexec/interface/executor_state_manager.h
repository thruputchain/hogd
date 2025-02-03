#pragma once

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>

#include "hogd/hogexec/interface/executor_state.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "rpc_types.hh"

namespace hogd {

class ExecutorStateManager {
public:
  ExecutorStateManager(DatabaseProxy &db_proxy) : db_proxy_(db_proxy) {}

  struct ExecutorContext {
    ExecutorContext(ExecutorMode mode, BlockID id, DatabaseProxy &db_proxy)
      : state_(mode), snapshot_cache_(id, db_proxy)
    {}

    ExecutorState state_;
    StorageSnapshotCache snapshot_cache_;
  };

  // This pointer will be valid for the lifetime of the block.
  // That is, while any transactions are running in that block id,
  // (while ExecutorState has active run leases)
  // the pointer will be valid.
  // These pointers are deleted only after await_finish() is called.
  // Acquires a run lease, so the returned pointer is guaranteed valid (if not
  // null) until release_run_lease is called on it.
  ExecutorContext *get_executor_context(BlockID block_id, ExecutorMode mode);

  void await_finish(BlockID block_id);
  void open_proposal(BlockID new_proposal_id);
  void open_validation(BlockID new_validation_id);

  void await_all_pending_blocks();
  void initiate_shutdown();

private:
  std::mutex mtx_;
  std::condition_variable cv_;

  std::map<BlockID, std::unique_ptr<ExecutorContext>> active_blocks_;
  bool shutdown_ = false;
  DatabaseProxy &db_proxy_;
};

} // namespace hogd
