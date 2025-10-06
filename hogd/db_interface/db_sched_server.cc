#include "hogd/db_interface/db_sched_server.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include "db_sched.hh"
#include "genesis.hh"
#include "hogd/base/types.h"
#include "rpc_types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"

namespace hogd {

x23::Async<DbSchedResult>
DbSchedServer::dbsched_create_snapshot(BlockID parent_id, BlockID new_id,
                                       BlockExecutionMode mode)
{
  if (block_manager_->add_child(parent_id, new_id, mode)) {
    co_return DbSchedResult::DbSchedResultOK;
  }
  co_return DbSchedResult::DbSchedResultError;
}

x23::Async<DbBlockResult>
DbSchedServer::dbsched_commit_snapshot(BlockID id)
{
  if (block_manager_->commit_block(id)) {
    //(geoff) TODO: Replace with actual BlockHash if available
    DbBlockResult result(DbSchedResult::DbSchedResultOK);
    // result.block_hash = ...; // Set block_hash if available
    co_return result;
  }
  DbBlockResult result(DbSchedResult::DbSchedResultError);
  co_return result;
}

x23::Async<DbBlockResult>
DbSchedServer::dbsched_close_snapshot(BlockID id)
{
  auto blk = block_manager_->get_block_data(id);
  if (!blk) {
    DbBlockResult result(DbSchedResult::DbSchedResultError);
    co_return result;
  }
  blk->close_block();
  DbBlockResult result(DbSchedResult::DbSchedResultOK);
  // TODO(geoff): Set block_hash if available
  // result.block_hash = ...; // Set block_hash if available
  co_return result;
}

x23::Async<DbSchedResult>
DbSchedServer::dbsched_abandon_snapshot(BlockID id)
{
  // TODO(geoff): Implement actual abandon logic if needed
  // For now, just return ok for stub. If and when this function can return a
  // status other than [DbSchedResultOK], the mempool will need to be updated
  // (please coordinate with jhumphri@ on this).
  co_return DbSchedResult::DbSchedResultOK;
}

uint16_t
DbSchedServer::start_server(uint16_t port_number)
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

  [](x23::CoroPoll &cp, DbSchedServer *server,
     x23::UniqueFd lfd) -> x23::Detached {
    co_await cp.pool_.bg();
    while (auto c = co_await cp.accept(lfd)) {
      x23::rpc_stream_logged(cp, *c)->server(server);
    }
  }(cp_, this, std::move(lfd));

  return port_number;
}

x23::Async<DbSchedResult>
DbSchedServer::dbsched_load_genesis_block(GenesisBlock const &genesis_block)
{
  if (block_manager_->initialize_genesis_block(genesis_block)) {
    co_return DbSchedResult::DbSchedResultOK;
  }
  co_return DbSchedResult::DbSchedResultError;
}

} // namespace hogd
