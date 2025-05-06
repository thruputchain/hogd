#include "hogd/db_interface/db_coord_server.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include "hogd/base/types.h"
#include "rpc_types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"

namespace hogd {

x23::Async<RPCError>
DbCoordServer::dbcoord_open_snapshot(BlockID parent_id, BlockID new_id)
{
  if (block_manager_->add_child(parent_id, new_id)) {
    co_return RPCError::kRPCNone;
  }
  co_return RPCError::kRPCBadBlockID;
}

x23::Async<RPCError>
DbCoordServer::dbcoord_commit_snapshot(BlockID id)
{
  if (block_manager_->commit_block(id)) {
    co_return RPCError::kRPCNone;
  }
  co_return RPCError::kRPCBadBlockID;
}

x23::Async<RPCError>
DbCoordServer::dbcoord_close_snapshot(BlockID id)
{
  auto blk = block_manager_->get_block_data(id);
  if (!blk) {
    co_return RPCError::kRPCBadBlockID;
  }
  blk->close_block();
  co_return RPCError::kRPCNone;
}

void
DbCoordServer::start_server(uint32_t port_number)
{
  {
    auto lfd = x23::tcplisten(port_number);
    if (!lfd) {
      perror("listen");
      exit(EXIT_FAILURE);
    }

    [](x23::CoroPoll &cp, DbCoordServer *server,
       x23::UniqueFd lfd) -> x23::Detached {
      co_await cp.pool_.bg();
      while (auto c = co_await cp.accept(lfd)) {
        x23::rpc_stream_logged(cp, *c).server(server);
      }
    }(cp_, this, std::move(lfd));
  }
}

} // namespace hogd
