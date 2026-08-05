#pragma once

#include <cstdlib>
#include <unordered_map>
#include <utility>

#include "coord_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "proposal.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/corosync.h"

namespace hogd {

// This is a fake coordinator server used by the tests.
class CoordinatorSchedRpc {
public:
  using rpc_service = CoordScheduler1;

  CoordinatorSchedRpc() = default;

  static x23::Async<void> coordscheduler_null() { co_return; }

  // We do not pass [txs] by reference just in case we call [co_await] in the
  // function. If the coroutine gets paused and another RPC of the same name is
  // delivered, the reference would become invalidated.
  static x23::Async<void>
  // NOLINTNEXTLINE(performance-unnecessary-value-param)
  coordscheduler_notify_vetted(const ParallelSignedTransactionBatch txs)
  {
    co_return;
  }

  void shut_down()
  {
    // TODO(jhumphri): Implement.
  }
};
static_assert(is_CoordScheduler1<CoordinatorSchedRpc *, x23::Async>);

} // namespace hogd
