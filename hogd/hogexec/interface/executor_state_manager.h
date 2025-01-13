#pragma once

#include <map>
#include <memory>

#include "hogd/hogexec/interface/executor_state.h"
#include "rpc.hh"
#include "x23/mvar.h"

namespace hogd {

class ExecutorStateManager {
public:
  // This pointer will be valid for the lifetime of the block.
  // That is, while any transactions are running in that block id,
  // (while ExecutorState has active run leases)
  // the pointer will be valid.
  // These pointers are deleted only after await_finish() is called.
  ExecutorState *get_executor_state(BlockID block_id);

  void await_finish(BlockID block_id);
  void open_proposal(BlockID new_proposal_id);
  void open_validation(BlockID new_validation_id);

private:
  x23::MVar<std::map<BlockID, std::unique_ptr<ExecutorState>>> active_blocks_;
};

} // namespace hogd
