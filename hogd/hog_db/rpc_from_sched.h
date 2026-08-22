#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <type_traits>

// generated x23 headers
#include "db_sched.hh"
#include "genesis.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/hog_db/block_id_manager.h"
#include "hogd/hog_db/block_tx.h"
#include "rpc_types.hh"
#include "types.hh"

#include "hogd/base/rpc_server.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/status.h"
#include "x23/async.h"

namespace hogd {

// Async server implementing the DbSched interface
class HogDbSchedRpc {
public:
  using rpc_service = DbSched1;

  // TODO(geoff): Some kind of check on ctx, to determine whether
  // the ledger has been initialized or do we have a truly empty database
  explicit HogDbSchedRpc(RPCContext &ctx) : ctx_(ctx)
  {
    if constexpr (is_debug_build()) {
      auto tip = ctx_.get_tip();
      if (tip.id != UINT64_MAX) {
        block_id_state_manager_.log_block_id_tip(tip.id);
      }
    }
  }

  static x23::Async<void> dbsched_null() { co_return; }

  // RPC methods
  x23::Async<HogAgeResults>
  dbsched_get_hog_ages(const ContractAddressBatch &addresses);

  x23::Async<DbSchedResult> dbsched_write_hog_ages(const HogAgeBatch &ages);

  x23::Async<DbAddressAuthDataBatchResult>
  dbsched_get_address_auth_data(BlockID block_id,
                                ContractAddressBatch accounts);

  x23::Async<DbSchedResult> dbsched_create_snapshot(BlockID parent_id,
                                                    BlockID new_id,
                                                    BlockExecutionMode mode);

  x23::Async<DbBlockResult> dbsched_commit_snapshot(BlockID id,
                                                    ConsensusValue value);
  x23::Async<DbSchedResult> dbsched_abandon_snapshot(BlockID id);
  x23::Async<DbBlockResult> dbsched_close_snapshot(BlockID id);

  x23::Async<DbSchedResult>
  dbsched_load_genesis_configuration(GenesisConfiguration genesis_configuration,
                                     ConsensusValue value);

  x23::Async<DbSchedResult> dbsched_load_genesis_configuration_v2(
      GenesisConfigurationV2 genesis_configuration, ConsensusValue value);

  static x23::Async<DbShardResult> dbsched_shard_restore(BlockHeight height);

  x23::Async<DbTipResult> dbsched_get_tip()
  {
    HogDbStatus<DbTip> ret = ctx_.get_db().read_dbtip();
    if (!ret) {
      co_return DbTipResult(DbSchedResult::DbSchedResultError);
    }
    DbTipResult result(DbSchedResult::DbSchedResultOK);
    result.db_tip() = *ret;
    co_return result;
  }

private:
  std::atomic<bool> shutdown_flag_ = false;
  RPCContext &ctx_;

  std::conditional_t<is_debug_build(), BlockIDStateManager,
                     UncheckedBlockIDStateManager>
      block_id_state_manager_;

  // commit_block is a helper function that performs all the steps necessary to
  // commit.
  // There must be an active reference to the blocktx.  This active reference
  // should be the last reference that is open for the btx..
  HogDbStatus<void> commit_block(BlockID id, ConsensusValue value,
                                 PerThreadARC<BlockTX> *btx,
                                 const StorageLongHash &db_hash);
};
static_assert(is_DbSched1<HogDbSchedRpc *, x23::Async>);

class HogDbSchedServer : public RpcServer<HogDbSchedRpc> {
public:
  explicit HogDbSchedServer(RPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
