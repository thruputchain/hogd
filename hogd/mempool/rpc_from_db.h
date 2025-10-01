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

class MempoolDbRpc {
public:
  using rpc_service = SchedDb1;

  MempoolDbRpc() {}

  // Disable copy and move.
  MempoolDbRpc(const MempoolDbRpc &) = delete;
  MempoolDbRpc &operator=(const MempoolDbRpc &) = delete;
  MempoolDbRpc(MempoolDbRpc &&) = delete;
  MempoolDbRpc &operator=(MempoolDbRpc &&) = delete;

  x23::Async<void> scheddb_intro_shard(ShardCard card)
  {
    info("db shard connected. {} total shards connected.", shards_.size() + 1);

    std::unique_lock lk(mutex_);
    shards_.push_back(card);
    co_return;
  }

  x23::Vec<ShardCard> shards()
  {
    std::unique_lock lk(mutex_);
    return shards_;
  }

  x23::Vec<db_sched_client_t> get_clients(x23::CoroPoll &cp)
  {
    x23::Vec<db_sched_client_t> ret;
    ret.reserve(shards_.size());

    for (const ShardCard &card : shards_) {
      x23::UniqueFd fd =
          x23::tcpconnect(server_address_to_sockaddr(card.address_for_sched));
      ret.push_back(
          x23::rpc_stream_logged(cp, fd.release())->client(DbSched1{}));
    }
    return ret;
  }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  std::mutex mutex_;
  x23::Vec<ShardCard> shards_;

  std::atomic<bool> shutdown_flag_ = false;
};
static_assert(is_SchedDb1<MempoolDbRpc *, x23::Async>);

} // namespace hogd
