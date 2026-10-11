#pragma once

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <type_traits>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "types.hh"

namespace hogd {

class TestSchedDBServer {
public:
  using rpc_service = SchedDb1;

  void scheddb_intro_shard(ShardCard const & /*unused*/) {};
  // Accumulates a block's run and publishes it once every chunk is in, so a
  // waiter never sees a partially delivered block. Chunks may arrive in any
  // order.
  void scheddb_hogs_transferred_chunk(HogsTransferredChunk const &chunk)
  {
    std::unique_lock<std::mutex> lock(mtx_);
    PendingRun &pending = pending_hogs_transferred_[chunk.block];
    for (const auto &delta : chunk.deltas) {
      pending.deltas[delta.address] = from_xdr(delta.hog_delta);
    }
    if (++pending.received_chunks == chunk.chunk_count) {
      assert(!hogs_transferred_.contains(chunk.block));
      hogs_transferred_[chunk.block] = std::move(pending.deltas);
      pending_hogs_transferred_.erase(chunk.block);
      cv_.notify_all();
    }
  };

  // access methods

  void wait_for_hog_transferred_height(BlockHeight height);
  void wait_for_changed_accounts_height(BlockHeight height);

  std::map<ContractAddress, int128_t> get_hogs_transferred(BlockHeight height);
  ContractAddressBatch get_changed_accounts(BlockHeight height);

private:
  std::mutex mtx_;
  std::condition_variable cv_;
  std::map<BlockHeight, std::map<ContractAddress, int128_t>> hogs_transferred_;
  struct PendingRun {
    std::map<ContractAddress, int128_t> deltas;
    uint32_t received_chunks = 0;
  };
  std::map<BlockHeight, PendingRun> pending_hogs_transferred_;
  std::map<BlockHeight, ContractAddressBatch> changed_accounts_;
};

static_assert(is_SchedDb1<TestSchedDBServer *, std::type_identity_t>);

} // namespace hogd
