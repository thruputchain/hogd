#pragma once

#include <memory>
#include <unordered_map>
#include <utility>

#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "rpc_types.hh"
#include "x23/mvar.h"

namespace hogd {

class ExecutorStateManager {
public:
  ExecutorStateManager(std::unique_ptr<DatabaseProxy> db_proxy)
    : db_proxy_(std::move(db_proxy))
  {}

  struct ExecutorContext {
    ExecutorContext(ExecutionMode mode, BlockID id, DatabaseProxy &db_proxy)
      : mode_(mode), db_proxy_(db_proxy), snapshot_cache_(id, db_proxy)
    {}

    ExecutionMode mode_;
    DatabaseProxy &db_proxy_;
    StorageSnapshotCache snapshot_cache_;
  };

  std::shared_ptr<ExecutorContext> get_executor_context(BlockID block_id,
                                                        ExecutionMode mode);

  void open_proposal(BlockID new_proposal_id);
  void open_validation(BlockID new_validation_id);
  void remove_block(BlockID block_id);

private:
  x23::MVar<std::unordered_map<BlockID, std::shared_ptr<ExecutorContext>>>
      active_blocks_;
  std::unique_ptr<DatabaseProxy> db_proxy_;
};

} // namespace hogd
