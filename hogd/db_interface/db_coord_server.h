#pragma once

#include <cassert>
#include <optional>

#include "db_coord.hh"
#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/genesis/genesis_block.h"
#include "nomination.hh"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd {

// Async server implementing the DbCoord interface
class DbCoordServer {
public:
  using rpc_service = DbCoord1;

  explicit DbCoordServer(BlockManager *block_manager)
    : block_manager_(block_manager)
  {
    assert(block_manager_);
  }

  // Start the server on the specified port
  // Returns the port number that the server is listening on
  uint16_t start_server(uint16_t port_number);

  static x23::Async<void> dbcoord_null() { co_return; }

  // RPC methods
  x23::Async<DbResult> dbcoord_create_snapshot(BlockID parent_id,
                                               BlockID new_id,
                                               BlockExecutionMode mode);
  static x23::Async<DbResult> dbcoord_open_snapshot(BlockID id)
  {
    co_return DbResult::DbResultError; // TODO(geoff): unimplemented
  }
  x23::Async<DbBlockResult> dbcoord_commit_snapshot(BlockID id);
  x23::Async<DbBlockResult> dbcoord_close_snapshot(BlockID id);
  static x23::Async<DbResult> dbcoord_abandon_snapshot(BlockID id);

  static x23::Async<DbCatchupResult> dbcoord_catchup(BlockHeight height)
  {
    co_return DbCatchupResult{
        DbResult::DbResultError}; // TODO(geoff): unimplemented
  }

  static x23::Async<DbTipResult> dbcoord_get_tip()
  {
    co_return DbTipResult{
        DbResult::DbResultError}; // TODO(geoff): unimplemented
  }

  x23::Async<DbResult>
  dbcoord_load_genesis_block(GenesisBlock const &genesis_block);

  static x23::Async<DbShardResult> dbcoord_shard_restore(BlockHeight height)
  {
    co_return DbShardResult::ShardOK;
  }

private:
  BlockManager *block_manager_;
  x23::ThreadPool pool_ = x23::ThreadPool(2);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);
};

static_assert(is_DbCoord1<DbCoordServer *, x23::Async>);

} // namespace hogd
