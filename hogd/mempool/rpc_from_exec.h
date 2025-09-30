#pragma once

#include <atomic>
#include <chrono>

#include "discovery.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/mempool/authentication.h"
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

  MempoolExecutorRpc() {}

  // Disable copy and move.
  MempoolExecutorRpc(const MempoolExecutorRpc &) = delete;
  MempoolExecutorRpc &operator=(const MempoolExecutorRpc &) = delete;
  MempoolExecutorRpc(MempoolExecutorRpc &&) = delete;
  MempoolExecutorRpc &operator=(MempoolExecutorRpc &&) = delete;

  x23::Async<void> schedexec_intro_exec(ExecCard card);

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  std::atomic<bool> shutdown_flag_ = false;
};
static_assert(is_SchedExec1<MempoolExecutorRpc *, x23::Async>);

class MempoolExecutorServer : public RpcServer<MempoolExecutorRpc> {};

} // namespace hogd
