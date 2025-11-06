#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "coord_sched.hh"
#include "exec_sched.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/base/time.h"
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

  static x23::Async<void>
  coordscheduler_notify_vetted(const SignedTransactionBatch &txs)
  {
    co_return;
  }

  x23::Async<void>
  coordscheduler_finish_nomination(const SchedulerNominationChunk &chunk)
  {
    x23::CoroGuard cg;
    co_await cg(nominations_lock_);

    CHECK(!nominations_.contains(chunk.chunk_idx));
    if (!nominations_.empty()) {
      const SchedulerNominationChunk &existing = nominations_.begin()->second;
      CHECK_EQ(chunk.chunk.block_id, existing.chunk.block_id);
    }
    CHECK_LT(nominations_.size(), chunk.total_chunks);
    nominations_[chunk.chunk_idx] = {};
    cg.reset();

    nominations_[chunk.chunk_idx] = chunk;
  }

  SchedulerNomination get_nomination()
  {
    CHECK(!nominations_.empty());
    CHECK_EQ(nominations_.size(), nominations_[0].total_chunks);

    // The block hash is in the last chunk.
    size_t last_chunk = nominations_[0].total_chunks - 1;
    SchedulerNomination ret = nominations_[last_chunk].chunk;
    for (size_t i = 0; i < nominations_[0].total_chunks; i++) {
      ret.nomination.transaction_hashes +=
          nominations_[i].chunk.nomination.transaction_hashes;
      ret.nomination.transaction_nominations +=
          nominations_[i].chunk.nomination.transaction_nominations;
    }

    nominations_.clear();
    return ret;
  }

  void shut_down()
  {
    // TODO(jhumphri): Implement.
  }

  void await_shutdown_request()
  {
    // TODO(jhumphri): Implement.
  }

private:
  x23::CoroLock nominations_lock_;
  std::unordered_map<unsigned int, SchedulerNominationChunk> nominations_;
};
static_assert(is_CoordScheduler1<CoordinatorSchedRpc *, x23::Async>);

} // namespace hogd
