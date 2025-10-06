#pragma once

#include <memory>
#include <unordered_map>
#include <utility>

#include "exec_sched.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/executor_context.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "rpc_types.hh"
#include "x23/mvar.h"

namespace hogd {

class ExecutorStateManager {
public:
  ExecutorStateManager(std::unique_ptr<DatabaseProxy> db_proxy,
                       std::unique_ptr<EventServerProxy> event_server_proxy)
    : db_proxy_(std::move(db_proxy)),
      event_server_proxy_(std::move(event_server_proxy))
  {}

  std::shared_ptr<ExecutorContext> get_executor_context(BlockID block_id);

  // return ExecResultOk if successful, or ExecResultSystemError if
  // the block_id already exists and is of a different mode.
  ExecResult open_proposal(BlockID new_proposal_id);
  ExecResult open_validation(BlockID new_validation_id);
  void remove_block(BlockID block_id);

private:
  ExecResult open_block(BlockID block_id, ExecutionMode mode);
  struct ContextEntry {
    uint32_t refcount = 0;
    std::shared_ptr<ExecutorContext> context = nullptr;
  };

  x23::MVar<std::unordered_map<BlockID, ContextEntry>> active_blocks_;
  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<EventServerProxy> event_server_proxy_;
};

} // namespace hogd
