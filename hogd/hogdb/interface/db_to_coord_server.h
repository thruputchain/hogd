#pragma once

#include "hogd/hogdb/interface/block_manager.h"
#include "rpc.hh"
#include "rpc_types.hh"
#include "x23/async.h"
#include "x23/rpc.h"

namespace hogd {

class DBToCoordServer {
public:
  DBToCoordServer(BlockManager &block_manager) : block_manager_(block_manager)
  {}
  using rpc_service = DatabaseToCoordInterfaceV1;

  x23::Async<RPCError> open_database_snapshot(BlockID parent_id,
                                              BlockID new_id);
  x23::Async<void> commit_snapshot(BlockID id);

  x23::Async<void> close_snapshot(BlockID id);

private:
  BlockManager &block_manager_;
};

static_assert(is_DatabaseToCoordInterfaceV1<DBToCoordServer *, x23::Async>);

} // namespace hogd
