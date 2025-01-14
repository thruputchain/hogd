#include "hogd/hogdb/interface/db_to_coord_server.h"

#include "rpc_types.hh"

namespace hogd {

x23::Async<RPCError>
DBToCoordServer::open_database_snapshot(BlockID parent_id, BlockID new_id)
{
  auto parent_state = block_manager_.get_block_data(parent_id);
  if (!parent_state) {
    co_return RPCError::kBadBlockID;
  }

  if (block_manager_.add_child(parent_id, new_id,
                               std::make_shared<BlockState>(parent_state))) {
    co_return RPCError::kNone;
  }
  co_return RPCError::kBadBlockID;
}

x23::Async<void>
DBToCoordServer::commit_snapshot(BlockID id)
{
  block_manager_.commit_block(id);
  co_return;
}

} // namespace hogd
