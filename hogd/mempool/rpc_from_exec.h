#pragma once

#include <atomic>
#include <chrono>

#include "discovery.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/mempool/authentication.h"
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

class MempoolExecutorServer {
public:
  using rpc_service = SchedExec1;

  MempoolExecutorServer() {}

  // Disable copy and move.
  MempoolExecutorServer(const MempoolExecutorServer &) = delete;
  MempoolExecutorServer &operator=(const MempoolExecutorServer &) = delete;
  MempoolExecutorServer(MempoolExecutorServer &&) = delete;
  MempoolExecutorServer &operator=(MempoolExecutorServer &&) = delete;

  x23::Async<void> schedexec_intro_exec(ExecCard card);

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  std::atomic<bool> shutdown_flag_ = false;
};

static_assert(is_SchedExec1<MempoolExecutorServer *, x23::Async>);

} // namespace hogd
