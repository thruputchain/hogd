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
    info("received chunk idx {}", chunk.chunk_idx);
    x23::CoroGuard cg;
    co_await cg(nominations_lock_);

    if (nominations_.contains(chunk.chunk_idx)) {
      info("about to fail for chunk idx {}", chunk.chunk_idx);
    }
    CHECK(!nominations_.contains(chunk.chunk_idx));
    if (!nominations_.empty()) {
      const SchedulerNominationChunk &existing = nominations_.begin()->second;
      CHECK_EQ(chunk.chunk.block_id, existing.chunk.block_id);
    }
    CHECK_LT(nominations_.size(), chunk.total_chunks);
    nominations_[chunk.chunk_idx] = chunk;
  }

  x23::Async<SchedulerNomination> get_nomination()
  {
    while (true) {
      x23::CoroGuard cg;
      co_await cg(nominations_lock_);
      if (!nominations_.empty() &&
          nominations_.size() == nominations_[0].total_chunks) {
        break;
      }
    }

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
    co_return ret;
  }

  x23::Async<std::pair<SchedulerNomination, size_t>> get_nomination_no_txs()
  {
    while (true) {
      x23::CoroGuard cg;
      co_await cg(nominations_lock_);
      if (!nominations_.empty() &&
          nominations_.size() == nominations_[0].total_chunks) {
        break;
      }
    }

    // The block hash is in the last chunk.
    size_t last_chunk = nominations_[0].total_chunks - 1;
    const SchedulerNomination &last = nominations_[last_chunk].chunk;

    SchedulerNomination ret{.block_id = last.block_id};
    Nomination &nom = ret.nomination;
    nom.proposer = last.nomination.proposer;
    nom.block_height = last.nomination.block_height;
    nom.block_hash = last.nomination.block_hash;
    nom.prev_block_hash = last.nomination.prev_block_hash;

    size_t num_txs = 0;
    for (const auto &[_, chunk] : nominations_) {
      num_txs += chunk.chunk.nomination.transaction_hashes.size();
    }

    nominations_.clear();
    co_return std::make_pair(std::move(ret), num_txs);
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
