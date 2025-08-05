// rpc_from_coord.cc - handles RPC messages from the coordinator.

#include "hogd/hogdb/rpc_from_coord.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

// generated x23 headers
#include "db_coord.hh"
#include "rpc_types.hh"

#include "hogd/base/types.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/status.h"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"

namespace hogd {

namespace {

template<typename T>
DbResult
to_db_result(HogDBStatus<T> ret)
{
  // TODO(abelay): the RPC error codes ought to be the same as or similar to
  // HogDBStatus; the current ones don't capture the range of possible errors.
  if (!ret)
    return DbResult::DbResultError;
  return DbResult::DbResultOK;
}

} // namespace

x23::Async<DbResult>
HogDBCoordServer::dbcoord_create_snapshot(BlockID parent_id, BlockID new_id,
                                          BlockExecutionMode mode)
{
  // TODO(abelay): the parent_id must be last committed snapshot. We could
  // check that here if we had the block height rather than the ID.
  co_return to_db_result(ctx_.open_block_tx(new_id));
}

x23::Async<DbBlockResult>
HogDBCoordServer::dbcoord_commit_snapshot(BlockID id)
{
  HogDBStatus<BlockTX *> ret = ctx_.open_block_tx(id);
  if (!ret)
    co_return DbBlockResult(to_db_result(ret));

  BlockTX &btx = **ret;
  co_return DbBlockResult(to_db_result(btx.commit()));
}

x23::Async<DbResult>
HogDBCoordServer::dbcoord_abandon_snapshot(BlockID id)
{
  HogDBStatus<BlockTX *> ret = ctx_.open_block_tx(id);
  if (!ret)
    co_return to_db_result(ret);

  BlockTX &btx = **ret;
  btx.abort();
  co_return DbResult::DbResultOK;
}

x23::Async<DbBlockResult>
HogDBCoordServer::dbcoord_close_snapshot(BlockID id)
{
  // TODO(abelay): Get rid of this. This should not be a separate RPC method.
  // Instead both commit and abort should implicitly close.

  ctx_.close_block_tx(id);
  co_return DbBlockResult(DbResult::DbResultOK);
}

void
HogDBCoordServer::start_server(uint16_t port)
{
  {
    auto lfd = x23::tcplisten(port);
    if (!lfd) {
      perror("listen");
      exit(EXIT_FAILURE);
    }

    [](x23::CoroPoll &cp, HogDBCoordServer *server,
       x23::UniqueFd lfd) -> x23::Detached {
      co_await cp.pool_.bg();
      while (auto c = co_await cp.accept(lfd)) {
        x23::rpc_stream_logged(cp, *c).server(server);
      }
    }(cp_, this, std::move(lfd));
  }
}

} // namespace hogd
