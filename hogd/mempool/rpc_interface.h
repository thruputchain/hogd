#pragma once

#include <atomic>
#include <limits>
#include <memory>
#include <random>
#include <utility>
#include <vector>

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

  MempoolRpcInterface(std::unique_ptr<RPCClient> &&db_client,
                      std::unique_ptr<RPCClient> &&exec_client)
    : db_client_(std::move(db_client)), exec_client_(std::move(exec_client))
  {}

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
  schedulercoord_validate(DbTip const &db_tip, Nomination const &nomination);

  // It is an error to shut down the mempool while there are transactions that
  // are still running.
  x23::Async<void> executorcoord_shutdown();

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  // TODO(jhumphri): We are using this function to generate unique block IDs.
  // This will need to be reworked if/when we have multiple threads processing
  // blocks.
  static uint64_t get_ns_since_epoch()
  {
    std::chrono::time_point<std::chrono::system_clock> now =
        std::chrono::system_clock::now();
    std::chrono::nanoseconds ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch());
    return ns.count();
  }

  std::unique_ptr<RPCClient> db_client_;
  std::unique_ptr<RPCClient> exec_client_;

  std::vector<SignedTransaction> vetted_txs_;

  std::atomic<bool> shutdown_flag_ = false;
};

static_assert(is_SchedulerCoord1<MempoolRpcInterface *, x23::Async>);

} // namespace hogd
