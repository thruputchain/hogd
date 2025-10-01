#pragma once

#include <atomic>
#include <chrono>

#include "discovery.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/mempool/authentication.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_server.h"
#include "nomination.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "sched_exec.hh"
#include "scheduler.h"
#include "transaction.hh"
#include "types.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/rpc.h"
#include "x23/vec.h"

namespace hogd {

class MempoolExecutorRpc {
public:
  using rpc_service = SchedExec1;

  MempoolExecutorRpc(MempoolDbRpc &db_rpc) : db_rpc_(db_rpc) {}

  // Disable copy and move.
  MempoolExecutorRpc(const MempoolExecutorRpc &) = delete;
  MempoolExecutorRpc &operator=(const MempoolExecutorRpc &) = delete;
  MempoolExecutorRpc(MempoolExecutorRpc &&) = delete;
  MempoolExecutorRpc &operator=(MempoolExecutorRpc &&) = delete;

  x23::Async<IntroExecResult> schedexec_intro_exec(ExecCard card)
  {
    info("executor connected. {} total executors connected.",
         execs_.size() + 1);

    std::unique_lock lk(mutex_);
    execs_.push_back(card);
    co_return IntroExecResult{.exec_id = execs_.size() - 1,
                              .shards = db_rpc_.shards()};
  }

  x23::Vec<ExecCard> executors()
  {
    std::unique_lock lk(mutex_);
    return execs_;
  }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  MempoolDbRpc &db_rpc_;

  std::mutex mutex_;
  x23::Vec<ExecCard> execs_;

  std::atomic<size_t> num_executors_ = 0;
  std::atomic<bool> shutdown_flag_ = false;
};
static_assert(is_SchedExec1<MempoolExecutorRpc *, x23::Async>);

} // namespace hogd
