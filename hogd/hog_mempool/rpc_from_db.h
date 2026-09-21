#pragma once

#include <memory>
#include <optional>
#include <utility>

#include "db_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hog_mempool/block_transfers.h"
#include "hogd/hog_mempool/registration.h"
#include "hogd/hog_mempool/scheduler.h"
#include "hogd/hog_mempool/shared.h"
#include "hogd/hog_mempool/transfer_run_assembler.h"
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

  // Collects the run of chunks hog_db sends for a committed block and hands
  // the scheduler the whole block once, so it keeps seeing one update per
  // height. The chunks may arrive in any order and on any connection;
  // scheduler_lock_, held through [cg] until handle_transfer is done,
  // serializes the assembler. A chunk's deltas move from the decoded record
  // into the block without a copy.
  x23::Async<void> scheddb_hogs_transferred_chunk(HogsTransferredChunk chunk)
  {
    auto cg = std::make_unique<x23::CoroGuard>();
    co_await (*cg)(scheduler_lock_);

    if (!scheduler_) {
      info("[MempoolDbRpc::scheddb_hogs_transferred_chunk] failed. This is ok "
           "and expected if the mempool is shutting down, so only ignore this "
           "issue in that case.");
      co_return;
    }
    std::optional<BlockTransfers> transfers =
        run_assembler_.push(std::move(chunk));
    if (transfers.has_value()) {
      // See the comment above if you change this.
      handle_transfer(std::move(*transfers), std::move(cg));
    }
    co_return;
  }

  x23::Detached handle_transfer(BlockTransfers transfers,
                                std::unique_ptr<x23::CoroGuard> cg)
  {
    co_await x23::ThreadPool::get_thread_pool()->bg();

    CHECK_PTR(scheduler_.get());
    co_await scheduler_->process_transfers(transfers);

    // We write the hog ages to the database down here because we need to update
    // the hog ages above first ([scheduler_->process_transfers()]) *and*
    // [scheduler_->process_transfers()] completes after block commit (when the
    // pending hog ages are applied) due to the
    // [scheduler_->pending_hog_age_update_lock_] lock acquired in
    // [schedulercoord_commit_block()] and [process_transfers()].
    //
    // These writes must also never overlap ramdb's sync() for the *next*
    // block's commit (Database::sync()'s precondition); otherwise ramdb
    // state is corrupted. [pending_hog_age_update_lock_] does not provide
    // that ordering — it is released before this write runs. hog_db
    // serializes its ramdb write passes with
    // [RPCContext::ramdb_write_pass_lock()], and ramdb's [write_record()]
    // CHECKs the invariant (#3397). As a side note, it is okay for
    // [commit_snapshot()] and [abandon_snapshot()] to race. We ensure that
    // [scheduler_->write_all_average_hog_ages_to_db()] completes before we
    // update [scheduler_->transfer_finished_], so we do not need to worry
    // about these hog age writes updating with the *next* block commit
    // either.
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
  TransferRunAssembler run_assembler_;
};
static_assert(is_SchedDb1<MempoolDbRpc *, x23::Async>);

} // namespace hogd
