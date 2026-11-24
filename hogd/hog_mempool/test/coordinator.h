#pragma once

#include <cstdlib>
#include <future>
#include <mutex>
#include <optional>
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

  std::future<ParallelSignedTransactionBatch> next_vetted_notification()
  {
    std::lock_guard lock(mutex_);
    return next_notification_.emplace().get_future();
  }

  // Own the incoming batch so the test can inspect it after the RPC returns.
  x23::Async<void>
  coordscheduler_notify_vetted(ParallelSignedTransactionBatch txs)
  {
    std::lock_guard lock(mutex_);
    if (next_notification_) {
      next_notification_->set_value(std::move(txs));
      next_notification_.reset();
    }
    co_return;
  }

  void shut_down()
  {
    // TODO(jhumphri): Implement.
  }

private:
  std::mutex mutex_;
  std::optional<std::promise<ParallelSignedTransactionBatch>>
      next_notification_;
};
static_assert(is_CoordScheduler1<CoordinatorSchedRpc *, x23::Async>);

} // namespace hogd
