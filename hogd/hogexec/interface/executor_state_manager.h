#pragma once

#include <memory>
#include <unordered_map>
#include <utility>

#include "hogd/base/worker_pool.h"
#include "hogd/hogexec/interface/executor_context.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "rpc_types.hh"
#include "x23/mvar.h"

namespace hogd {

class ExecutorStateManager {
public:
  ExecutorStateManager(std::unique_ptr<DatabaseProxy> db_proxy, WorkerPool &main_worker_pool)
    : main_worker_pool_(main_worker_pool), db_proxy_(std::move(db_proxy))
  {}

  std::shared_ptr<ExecutorContext> get_executor_context(BlockID block_id);

  void open_proposal(BlockID new_proposal_id);
  void open_validation(BlockID new_validation_id);
  void remove_block(BlockID block_id);

private:
  WorkerPool &main_worker_pool_;
  x23::MVar<std::unordered_map<BlockID, std::shared_ptr<ExecutorContext>>>
      active_blocks_;
  std::unique_ptr<DatabaseProxy> db_proxy_;
};

} // namespace hogd
