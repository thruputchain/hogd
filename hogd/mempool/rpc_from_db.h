#pragma once

#include <atomic>
#include <chrono>
#include <utility>

#include "discovery.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/mempool/authentication.h"
#include "hogd/mempool/registration.h"
#include "hogd/mempool/scheduler.h"
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

  MempoolDbRpc() = default;

  // Disable copy and move.
  MempoolDbRpc(const MempoolDbRpc &) = delete;
  MempoolDbRpc &operator=(const MempoolDbRpc &) = delete;
  MempoolDbRpc(MempoolDbRpc &&) = delete;
  MempoolDbRpc &operator=(MempoolDbRpc &&) = delete;

  static x23::Async<void> scheddb_intro_shard(ShardCard card)
  {
    // We add 1 because we print this before we actually add the shard below.
    info("db shard with card {} connected. {} total shards connected.",
         x23::show(card),
         Registration::registration().shards_registered(/*cards=*/nullptr) + 1);
    Registration::registration().register_shard(card);
    co_return;
  }

  x23::Async<void> scheddb_hogs_transferred(HogsTransferred transfers)
  {
    CHECK_PTR(scheduler_.get());

    for (const Delta &d : transfers.deltas) {
      // std::println("scheddb_hogs_transferred, block height {}, account {},
      // num "
      //              "hogs transferred {}",
      //              transfers.block, d.address, from_xdr(d.hog_delta));
      int128_t hog_delta = from_xdr(d.hog_delta);
      CHECK_NE(hog_delta, 0L);

      auto hog_delta_abs = static_cast<uint128_t>(std::abs(hog_delta));

      if (hog_delta >= 0L) {
        co_await scheduler_->process_received_hogs(d.address, hog_delta_abs,
                                                   transfers.block);
      }
      else {
        co_await scheduler_->process_deducted_hogs(d.address, hog_delta_abs);
      }
    }
  }

  void register_scheduler(std::shared_ptr<Scheduler> scheduler)
  {
    scheduler_ = std::move(scheduler);
  }

  static x23::Vec<db_sched_client_t> get_clients(x23::CoroPoll &cp)
  {
    x23::Vec<ShardCard> cards;
    Registration::registration().shards_registered(&cards);

    x23::Vec<db_sched_client_t> ret;
    ret.reserve(cards.size());

    for (const ShardCard &card : cards) {
      x23::UniqueFd fd =
          x23::tcpconnect(server_address_to_sockaddr(card.address_for_sched));
      ret.push_back(
          x23::rpc_stream_logged(cp, fd.release())->client(DbSched1{}));
    }
    return ret;
  }

  void shut_down()
  {
    // TODO(jhumphri): Implement.
    shutdown_flag_ = true;
  }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  std::atomic<bool> shutdown_flag_ = false;

  std::shared_ptr<Scheduler> scheduler_;
};
static_assert(is_SchedDb1<MempoolDbRpc *, x23::Async>);

} // namespace hogd
