#include "hogd/mempool/rpc_interface.h"

#include <atomic>
#include <cassert>

#include "db_coord.hh"
#include "exec_coord.hh"
#include "hogd/base/types.h"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

x23::Async<void>
MempoolRpcInterface::schedulercoord_submit_unvetted(
    SignedTransactionBatch const &batch)
{
  co_return;
}

x23::Async<void>
MempoolRpcInterface::schedulercoord_submit_vetted(
    ValidatorID validator_id, SignedTransactionBatch const &batch)
{
  co_return;
}

x23::Async<SchedulerNominationResult>
MempoolRpcInterface::schedulercoord_nominate(DbTip const &db_tip)
{
  co_return SchedulerNominationResult(
      SchedulerNominationOutcome::SchedulerNominationError);
}

x23::Async<SchedulerValidationResult>
MempoolRpcInterface::schedulercoord_validate(Nomination const &nomination)
{
  co_return SchedulerValidationResult(
      SchedulerValidationOutcome::SchedulerValidationInvalid);
}

x23::Async<void>
MempoolRpcInterface::executorcoord_shutdown()
{
  shutdown_flag_.store(true, std::memory_order_release);
  co_return;
}

} // namespace hogd
