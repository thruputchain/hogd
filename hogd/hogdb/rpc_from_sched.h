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
  explicit HogDBSchedServer(RPCContext &ctx) : ctx_(ctx) {}

  // Start the server on the specified port
  void start_server(uint16_t port);

  static x23::Async<void> dbsched_null() { co_return; }

  // RPC methods
  x23::Async<HogAgeResult>
  dbsched_get_hog_ages(const ContractAddressBatch &addresses);

  x23::Async<DbSchedResult> dbsched_write_hog_ages(const HogAgeBatch &ages);

  static x23::Async<DbAccountAuthDataBatchResult>
  dbsched_get_account_auth_data(BlockID block_id,
                                ContractAddressBatch accounts);

  uint16_t port();

private:
  RPCContext &ctx_;
  x23::ThreadPool pool_ = x23::ThreadPool(2);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  // protect port_ during initialization
  std::mutex mtx_;
  std::condition_variable cv_;
  std::optional<uint16_t> port_;
};
static_assert(is_DbSched1<HogDBSchedServer *, x23::Async>);

} // namespace hogd
