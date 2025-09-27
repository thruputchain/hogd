#pragma once

#include <cassert>
#include <condition_variable>
#include <mutex>
#include <optional>

// generated x23 headers
#include "db_sched.hh"
#include "genesis.hh"
#include "rpc_types.hh"
#include "types.hh"

#include "hogd/base/types.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/status.h"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd {

// Async server implementing the DbSched interface
class HogDBSchedServer {
public:
  using rpc_service = DbSched1;

  // TODO(geoff): Some kind of check on ctx, to determine whether
  // the ledger has been initialized or do we have a truly empty database
  explicit HogDBSchedServer(RPCContext &ctx) : ctx_(ctx)
  {
    if constexpr (is_debug_build()) {
      if (ctx_.get_tip().id != UINT64_MAX) {
        block_id_state_manager_.log_block_id_tip(ctx_.get_tip().id);
      }
    }
  }

  // Start the server on the specified port
  void start_server(uint16_t port);

  static x23::Async<void> dbsched_null() { co_return; }

  // RPC methods
  x23::Async<HogAgeResult>
  dbsched_get_hog_ages(const ContractAddressBatch &addresses);

  x23::Async<DbSchedResult> dbsched_write_hog_ages(const HogAgeBatch &ages);

  x23::Async<DbAccountAuthDataBatchResult>
  dbsched_get_account_auth_data(BlockID block_id,
                                ContractAddressBatch accounts);

  // RPC methods
  x23::Async<DbSchedResult> dbsched_create_snapshot(BlockID parent_id,
                                                    BlockID new_id,
                                                    BlockExecutionMode mode);

  x23::Async<DbBlockResult> dbsched_commit_snapshot(BlockID id);
  x23::Async<DbSchedResult> dbsched_abandon_snapshot(BlockID id);
  x23::Async<DbBlockResult> dbsched_close_snapshot(BlockID id);

  static x23::Async<DbCatchupResult> dbsched_catchup(BlockHeight height)
  {
    co_return DbCatchupResult{
        DbSchedResult::DbSchedResultError}; // TODO(geoff): unimplemented
  }

  x23::Async<DbTipResult> dbsched_get_tip();

  x23::Async<DbSchedResult>
  dbsched_load_genesis_block(GenesisBlock genesis_block);

  static x23::Async<DbShardResult> dbsched_shard_restore(BlockHeight height);

  uint16_t port();

private:
  RPCContext &ctx_;
  x23::ThreadPool pool_ = x23::ThreadPool(2);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  // protect port_ during initialization
  std::mutex mtx_;
  std::condition_variable cv_;
  std::optional<uint16_t> port_;

  std::conditional_t<is_debug_build(), BlockIDStateManager,
                     UncheckedBlockIDStateManager>
      block_id_state_manager_;

  // commit_block is a helper function that performs all the steps necessary to
  // commit.
  HogDBStatus<void> commit_block(BlockID id);
};
static_assert(is_DbSched1<HogDBSchedServer *, x23::Async>);

} // namespace hogd
