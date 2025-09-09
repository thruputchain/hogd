#pragma once

#include <cassert>
#include <condition_variable>
#include <mutex>
#include <optional>

// generated x23 headers
#include "db_coord.hh"
#include "genesis.hh"
#include "nomination.hh"
#include "rpc_types.hh"

#include "hogd/base/types.h"
#include "hogd/hogdb/rpc_context.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd {

// Async server implementing the DbCoord interface
class HogDBCoordServer {
public:
  using rpc_service = DbCoord1;

  // TODO(geoff): Some kind of check on ctx, to determine whether
  // the ledger has been initialized or do we have a truly empty database
  explicit HogDBCoordServer(RPCContext &ctx) : ctx_(ctx) {}

  // Start the server on the specified port
  void start_server(uint16_t port);

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
  x23::Async<DbResult> dbcoord_abandon_snapshot(BlockID id);
  x23::Async<DbBlockResult> dbcoord_close_snapshot(BlockID id);

  static x23::Async<DbCatchupResult> dbcoord_catchup(BlockHeight height)
  {
    co_return DbCatchupResult{
        DbResult::DbResultError}; // TODO(geoff): unimplemented
  }

  x23::Async<DbTipResult> dbcoord_get_tip();

  x23::Async<HogAgeResult>
  dbcoord_get_hog_ages(const ContractAddressBatch &addresses);

  x23::Async<DbResult> dbcoord_write_hog_ages(const HogAgeBatch &ages);

  x23::Async<DbResult> dbcoord_load_genesis_block(GenesisBlock genesis_block);

  static x23::Async<DbShardResult> dbcoord_shard_restore(BlockHeight height);

  uint16_t port();

private:
  RPCContext &ctx_;
  x23::ThreadPool pool_ = x23::ThreadPool(2);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  std::atomic<bool> is_initialized_ = false;

  // protect port_ during initialization
  std::mutex mtx_;
  std::condition_variable cv_;
  std::optional<uint16_t> port_;

  // commit_block is a helper function that performs all the steps necessary to
  // commit.
  HogDBStatus<void> commit_block(BlockID id);
};
static_assert(is_DbCoord1<HogDBCoordServer *, x23::Async>);

} // namespace hogd
