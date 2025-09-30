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

class MempoolDbServer {
public:
  using rpc_service = SchedDb1;

  MempoolDbServer() {}

  // Disable copy and move.
  MempoolDbServer(const MempoolDbServer &) = delete;
  MempoolDbServer &operator=(const MempoolDbServer &) = delete;
  MempoolDbServer(MempoolDbServer &&) = delete;
  MempoolDbServer &operator=(MempoolDbServer &&) = delete;

  x23::Async<void> scheddb_intro_shard(ShardCard card);

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  std::atomic<bool> shutdown_flag_ = false;
};

static_assert(is_SchedDb1<MempoolDbServer *, x23::Async>);

} // namespace hogd
