#pragma once

#include <cassert>
#include <optional>

#include "db_sched.hh"
#include "genesis.hh"
#include "hogd/base/rpc_server.h"
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

// Async server implementing the DbSched interface
class DbSchedRpc {
public:
  using rpc_service = DbSched1;

  explicit DbSchedRpc(BlockManager *block_manager)
    : block_manager_(block_manager)
  {
    assert(block_manager_);
  }

  // RPC methods
  static x23::Async<void> dbsched_null() { co_return; }

  static x23::Async<HogAgeResult>
  dbsched_get_hog_ages(const ContractAddressBatch &addresses)
  {
    co_return HogAgeResult(DbSchedResult::DbSchedResultError);
  }

  static x23::Async<DbSchedResult>
  dbsched_write_hog_ages(const HogAgeBatch &ages)
  {
    co_return DbSchedResult::DbSchedResultError;
  }

  static x23::Async<DbAccountAuthDataBatchResult>
  dbsched_get_account_auth_data(BlockID block_id, ContractAddressBatch accounts)
  {
    co_return DbAccountAuthDataBatchResult(DbSchedResult::DbSchedResultError);
  }

  x23::Async<DbSchedResult> dbsched_create_snapshot(BlockID parent_id,
                                                    BlockID new_id,
                                                    BlockExecutionMode mode);
  static x23::Async<DbSchedResult> dbsched_open_snapshot(BlockID id)
  {
    co_return DbSchedResult::DbSchedResultError; // TODO(geoff): unimplemented
  }
  x23::Async<DbBlockResult> dbsched_commit_snapshot(BlockID id);
  x23::Async<DbBlockResult> dbsched_close_snapshot(BlockID id);
  static x23::Async<DbSchedResult> dbsched_abandon_snapshot(BlockID id);

  static x23::Async<DbTipResult> dbsched_get_tip()
  {
    co_return DbTipResult{
        DbSchedResult::DbSchedResultError}; // TODO(geoff): unimplemented
  }

  x23::Async<DbSchedResult>
  dbsched_load_genesis_block(GenesisBlock const &genesis_block);

  void shut_down()
  {
    // TODO(jhumphri): Implement.
  }

  void await_shutdown_request()
  {
    // TODO(jhumphri): Implement.
  }

private:
  BlockManager *block_manager_;
};
static_assert(is_DbSched1<DbSchedRpc *, x23::Async>);

class DbSchedServer : public RpcServer<DbSchedRpc> {
public:
  DbSchedServer(BlockManager *block_manager) : RpcServer(block_manager) {}
};

} // namespace hogd
