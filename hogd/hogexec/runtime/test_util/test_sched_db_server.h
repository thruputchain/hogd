#pragma once

#include <cassert>
#include <condition_variable>
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
  void scheddb_hogs_transferred(HogsTransferred const &hogs)
  {
    std::map<ContractAddress, int128_t> hogs_transferred;
    for (const auto &delta : hogs.deltas) {
      hogs_transferred[delta.address] = from_xdr(delta.hog_delta);
    }
    register_height(hogs.block, hogs_transferred);
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
  std::map<BlockHeight, ContractAddressBatch> changed_accounts_;

  void register_height(BlockHeight height,
                       std::map<ContractAddress, int128_t> hogs_transferred)
  {
    std::unique_lock<std::mutex> lock(mtx_);
    assert(!hogs_transferred_.contains(height));
    hogs_transferred_[height] = std::move(hogs_transferred);
    cv_.notify_all();
  }
};

static_assert(is_SchedDb1<TestSchedDBServer *, std::type_identity_t>);

} // namespace hogd
