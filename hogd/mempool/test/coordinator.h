#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "coord_sched.hh"
#include "exec_sched.hh"
#include "hogd/base/rpc_server.h"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/threadpool.h"

namespace hogd {

// This is a fake coordinator server used by the tests.
class CoordinatorSchedRpc {
public:
  using rpc_service = CoordScheduler1;

  static x23::Async<void> coordscheduler_null() { co_return; }

  static x23::Async<IntroSchedResult>
  coordscheduler_intro_sched(SchedulerCard card)
  {
    co_return IntroSchedResult{.current_block_id = 0};
  };

  static x23::Async<void>
  coordscheduler_notify_vetted(const SignedTransactionBatch &txs)
  {
    co_return;
  }

  void shut_down()
  {
    // TODO(jhumphri): Implement.
  }

  void await_shutdown_request()
  {
    // TODO(jhumphri): Implement.
  }
};
static_assert(is_CoordScheduler1<CoordinatorSchedRpc *, x23::Async>);

class CoordinatorSchedServer : public RpcServer<CoordinatorSchedRpc> {};

} // namespace hogd
