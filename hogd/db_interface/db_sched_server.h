#pragma once

#include <cassert>

#include "db_sched.hh"
#include "genesis.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/db_interface/block_manager.h"
#include "rpc_types.hh"
#include "x23/async.h"

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

  static x23::Async<HogAgeResults>
  dbsched_get_hog_ages(const ContractAddressBatch &addresses)
  {
    HogAgeResult result(DbSchedResult::DbSchedResultError);
    co_return x23::Vec<HogAgeResult>(addresses.size(), result);
  }

  static x23::Async<DbSchedResult>
  dbsched_write_hog_ages(const HogAgeBatch &ages)
  {
    co_return DbSchedResult::DbSchedResultError;
  }

  static x23::Async<DbAddressAuthDataBatchResult>
  dbsched_get_address_auth_data(BlockID block_id, ContractAddressBatch accounts)
  {
    co_return DbAddressAuthDataBatchResult(DbSchedResult::DbSchedResultError);
  }

  x23::Async<DbSchedResult> dbsched_create_snapshot(BlockID parent_id,
                                                    BlockID new_id,
                                                    BlockExecutionMode mode);
  static x23::Async<DbSchedResult> dbsched_open_snapshot(BlockID id)
  {
    co_return DbSchedResult::DbSchedResultError; // TODO(geoff): unimplemented
  }
  x23::Async<DbBlockResult> dbsched_commit_snapshot(BlockID id,
                                                    ConsensusValue value);
  x23::Async<DbBlockResult> dbsched_close_snapshot(BlockID id);
  static x23::Async<DbSchedResult> dbsched_abandon_snapshot(BlockID id);

  x23::Async<DbSchedResult>
  dbsched_load_genesis_configuration(GenesisConfiguration genesis_configuration,
                                     ConsensusValue value);

  static x23::Async<DbTipResult> dbsched_get_tip()
  {
    co_return DbTipResult(DbSchedResult::DbSchedResultError);
  }

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
