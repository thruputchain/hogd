#include "hogd/db_interface/db_coord_server.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include "db_coord.hh"
#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/genesis/genesis_block.h"
#include "rpc_types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"

namespace hogd {

x23::Async<DbResult>
DbCoordServer::dbcoord_create_snapshot(BlockID parent_id, BlockID new_id,
                                       BlockExecutionMode mode)
{
  if (block_manager_->add_child(parent_id, new_id, mode)) {
    co_return DbResult::DbResultOK;
  }
  co_return DbResult::DbResultError;
}

x23::Async<DbBlockResult>
DbCoordServer::dbcoord_commit_snapshot(BlockID id)
{
  if (block_manager_->commit_block(id)) {
    //(geoff) TODO: Replace with actual BlockHash if available
    DbBlockResult result(DbResult::DbResultOK);
    // result.block_hash = ...; // Set block_hash if available
    co_return result;
  }
  DbBlockResult result(DbResult::DbResultError);
  co_return result;
}

x23::Async<DbBlockResult>
DbCoordServer::dbcoord_close_snapshot(BlockID id)
{
  auto blk = block_manager_->get_block_data(id);
  if (!blk) {
    DbBlockResult result(DbResult::DbResultError);
    co_return result;
  }
  blk->close_block();
  DbBlockResult result(DbResult::DbResultOK);
  // TODO(geoff): Set block_hash if available
  // result.block_hash = ...; // Set block_hash if available
  co_return result;
}

x23::Async<DbResult>
DbCoordServer::dbcoord_abandon_snapshot(BlockID id)
{
  // TODO(geoff): Implement actual abandon logic if needed
  // For now, just return ok for stub. If and when this function can return a
  // status other than [DbResultOK], the mempool will need to be updated
  // (please coordinate with jhumphri@ on this).
  co_return DbResult::DbResultOK;
}

uint16_t
DbCoordServer::start_server(uint16_t port_number)
{
  x23::UniqueFd lfd = x23::tcplisten(port_number);
  if (!lfd) {
    perror("listen");
    exit(EXIT_FAILURE);
  }
  // If the [port_number] parameter is 0, then the kernel chooses the port
  // number for us. Thus, in that case, [port_number] will be assigned a
  // nonzero value below.
  port_number = x23::get_port(x23::local_addr(lfd.get()));

  [](x23::CoroPoll &cp, DbCoordServer *server,
     x23::UniqueFd lfd) -> x23::Detached {
    co_await cp.pool_.bg();
    while (auto c = co_await cp.accept(lfd)) {
      x23::rpc_stream_logged(cp, *c).server(server);
    }
  }(cp_, this, std::move(lfd));

  return port_number;
}

x23::Async<DbResult>
DbCoordServer::dbcoord_load_genesis_block(GenesisBlock const &genesis_block)
{
  block_manager_->initialize_genesis_block(genesis_block);
  co_return DbResult::DbResultOK;
}

} // namespace hogd
