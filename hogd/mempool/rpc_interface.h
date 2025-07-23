#pragma once

#include <atomic>
#include <memory>
#include <utility>

#include "exec_coord.hh"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/hogexec/interface/executor_state_manager.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

// The coordinator uses this to access the scheduler/mempool.
class MempoolRpcInterface {
public:
  using rpc_service = SchedulerCoord1;

  MempoolRpcInterface() = default;

  // Disable copy and move
  MempoolRpcInterface(MempoolRpcInterface const &) = delete;
  MempoolRpcInterface &operator=(MempoolRpcInterface const &) = delete;
  MempoolRpcInterface(MempoolRpcInterface &&) = delete;
  MempoolRpcInterface &operator=(MempoolRpcInterface &&) = delete;

  static x23::Async<void> schedulercoord_null() { co_return; }

  x23::Async<void>
  schedulercoord_submit_unvetted(SignedTransactionBatch const &batch);
  x23::Async<void>
  schedulercoord_submit_vetted(ValidatorID validator_id,
                               SignedTransactionBatch const &batch);

  x23::Async<SchedulerNominationResult>
  schedulercoord_nominate(DbTip const &db_tip);

  x23::Async<SchedulerValidationResult>
  schedulercoord_validate(Nomination const &nomination);

  // It is an error to shut down the mempool while there are transactions that
  // are still running.
  x23::Async<void> executorcoord_shutdown();

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  std::atomic<bool> shutdown_flag_ = false;
};

static_assert(is_SchedulerCoord1<MempoolRpcInterface *, x23::Async>);

} // namespace hogd
