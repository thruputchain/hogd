#pragma once

#include <memory>
#include <utility>

#include "db_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/mempool/registration.h"
#include "hogd/mempool/scheduler.h"
#include "hogd/mempool/shared.h"
#include "sched_db.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/format.h"
#include "x23/threadpool.h"
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
    auto cg = std::make_unique<x23::CoroGuard>();
    co_await (*cg)(scheduler_lock_);

    if (scheduler_) {
      // See the comment above if you change this.
      handle_transfer(transfers, std::move(cg));
    }
    else {
      info("[MempoolDbRpc::scheddb_hogs_transferred] failed. This is ok and "
           "expected if the mempool is shutting down, so only ignore this "
           "issue in that case.");
    }
    co_return;
  }

  x23::Detached handle_transfer(HogsTransferred transfers,
                                std::unique_ptr<x23::CoroGuard> cg)
  {
    co_await x23::ThreadPool::get_thread_pool()->bg();

    CHECK_PTR(scheduler_.get());

    // Wait until genesis has been loaded on every db shard before touching hog
    // ages. [process_transfers] -> [read_average_hog_ages] shards reads by
    // address, and the genesis fan-out is sequential, so the first shard's
    // genesis-block deltas (which include accounts owned by shards that have
    // not yet run genesis, e.g. [kDeployContractAddress]) would otherwise read
    // a not-yet-populated shard and fail. Once set, this barrier stays open, so
    // steady-state blocks do not wait.
    while (!scheduler_->all_shards_genesis_ready_.load()) {
      co_await scheduler_->all_shards_genesis_ready_.wait(false);
    }

    // The barrier is also opened when the genesis fan-out *fails* on a shard,
    // so that we do not hang here forever. In that case the sharded db is only
    // partially initialized; drop the transfer rather than reading it (which
    // would trip the missing-hog-age CHECK). The run is being aborted anyway.
    if (scheduler_->genesis_load_failed_.load(std::memory_order_acquire)) {
      info("[MempoolDbRpc::handle_transfer] genesis load failed on a shard; "
           "dropping transfer for block {} (run is aborting).",
           transfers.block);
      co_return;
    }

    co_await scheduler_->process_transfers(transfers);

    // We write the hog ages to the database down here because we need to update
    // the hog ages above first ([scheduler_->process_transfers()]) *and*
    // [scheduler_->process_transfers()] completes after block commit (when the
    // pending hog ages are applied) due to the
    // [scheduler_->pending_hog_age_update_lock_] lock acquired in
    // [schedulercoord_commit_block()] and [process_transfers()].
    //
    // Additionally, we cannot have db writes race with snapshot commit as
    // otherwise ramdb state will be corrupted (based on a conversation with
    // Geoff and Ryan). Thus, we do these writes after the snapshot commit is
    // complete (i.e., all [commit_handles] have been co_await'd). As a side
    // note, it is okay for [commit_snapshot()] and [abandon_snapshot()] to
    // race. We ensure that [scheduler_->write_all_average_hog_ages_to_db()]
    // completes before we update [scheduler_->transfer_finished_], so we do not
    // need to worry about these hog age writes updating with the *next* block
    // commit either.
    co_await scheduler_->write_all_average_hog_ages_to_db();

    scheduler_->transfer_finished_.store(transfers.block);
    scheduler_->transfer_finished_.broadcast(&default_threadpool());
    // @geoff to @jhumphri: Do we need the cg = nullptr here? The cg gets
    // dropped automatically by going out of scope at the end of the function
    // body
    cg = nullptr; // cg.reset(); but CoroGuard also has a reset() method
  }

  x23::Async<void> register_scheduler(std::shared_ptr<Scheduler> scheduler)
  {
    x23::CoroGuard cg;
    co_await cg(scheduler_lock_);

    CHECK_PTR(scheduler.get());
    scheduler_ = std::move(scheduler);
  }

  x23::Async<void> reset_scheduler()
  {
    x23::CoroGuard cg;
    co_await cg(scheduler_lock_);

    CHECK_PTR(scheduler_.get());
    scheduler_.reset();
  }

  static x23::Async<x23::Vec<db_sched_client_t>> get_clients()
  {
    x23::XdrVec<ShardCard> cards;
    Registration::registration().shards_registered(&cards);

    x23::Vec<db_sched_client_t> ret;
    ret.reserve(cards.size());

    for (const ShardCard &card : cards) {
      auto client = co_await create_client<DbSched1>(
          server_address_to_sockaddr(card.address_for_sched),
          /*retry=*/false,
          /*max_message_size=*/kBulkRpcMaxMessageSize);
      if (!client)
        panic("rpc_from_db get_clients connect failed: {}",
              client.error().error_code().message());
      ret.push_back(std::move(*client));
    }
    co_return ret;
  }

  void shut_down()
  {
    // TODO(jhumphri): Implement.
  }

private:
  x23::CoroLock scheduler_lock_;
  std::shared_ptr<Scheduler> scheduler_;
};
static_assert(is_SchedDb1<MempoolDbRpc *, x23::Async>);

} // namespace hogd
