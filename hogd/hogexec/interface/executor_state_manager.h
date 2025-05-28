#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>

#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "rpc_types.hh"
#include "x23/mvar.h"

namespace hogd {

class ExecutorStateManager {
public:
  ExecutorStateManager(DatabaseProxy &db_proxy) : db_proxy_(db_proxy) {}

  struct ExecutorContext {
    ExecutorContext(ExecutionMode mode, BlockID id, DatabaseProxy &db_proxy)
      : mode_(mode), snapshot_cache_(id, db_proxy)
    {}

    ExecutionMode mode_;
    StorageSnapshotCache snapshot_cache_;
  };

  std::shared_ptr<ExecutorContext> get_executor_context(BlockID block_id,
                                                        ExecutionMode mode);

  void open_proposal(BlockID new_proposal_id);
  void open_validation(BlockID new_validation_id);
  void remove_block(BlockID block_id);

  void initiate_shutdown() { shutdown_ = true; }

  ~ExecutorStateManager() { await_shutdown(); }

private:
  void await_shutdown();

  std::atomic<bool> shutdown_ = false;
  x23::MVar<std::unordered_map<BlockID, std::shared_ptr<ExecutorContext>>>
      active_blocks_;
  DatabaseProxy &db_proxy_;
};

} // namespace hogd
