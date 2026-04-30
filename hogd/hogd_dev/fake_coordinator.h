#pragma once

#include "coord_sched.hh"
#include "hogd/base/types.h"
#include "nomination.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

class DevCoordinatorSchedRpc {
public:
  using rpc_service = CoordScheduler1;

  DevCoordinatorSchedRpc() = default;

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
static_assert(is_CoordScheduler1<DevCoordinatorSchedRpc *, x23::Async>);

} // namespace hogd
