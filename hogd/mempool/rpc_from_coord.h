#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>

#include "coord_sched.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "exec_sched.hh"
#include "genesis.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/mempool/authentication.h"
#include "hogd/mempool/registration.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_from_event.h"
#include "hogd/mempool/rpc_from_exec.h"
#include "hogd/mempool/shared.h"
#include "hogd/version.h"
#include "new_scheduler.h"
#include "nomination.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

// The mempool accepts transactions from the coordinator along with block
// nomination and validation requests.
class MempoolCoordinatorRpc {
public:
  using rpc_service = SchedulerCoord1;

  // The maximum number of txs in a block.
  static constexpr size_t kMaxBlockSize = 1'000'000;
  static constexpr size_t kNumSchedulerIterations = 10;
  static_assert(kMaxBlockSize % kNumSchedulerIterations == 0);

  static constexpr size_t kChunkSize = 10;
  static constexpr size_t kNumClientsPerExecutor = 10;

  MempoolCoordinatorRpc(std::shared_ptr<MempoolDbRpc> db_rpc)
    : db_rpc_(std::move(db_rpc))
  {}

  // Disable copy and move.
  MempoolCoordinatorRpc(const MempoolCoordinatorRpc &) = delete;
  MempoolCoordinatorRpc &operator=(const MempoolCoordinatorRpc &) = delete;
  MempoolCoordinatorRpc(MempoolCoordinatorRpc &&) = delete;
  MempoolCoordinatorRpc &operator=(MempoolCoordinatorRpc &&) = delete;

  std::shared_ptr<NewScheduler> scheduler() { return scheduler_; }
  Authentication &authentication() { return *authentication_; }

  static coord_sched_client_t get_client()
  {
    CoordCard card;
    CHECK(Registration::registration().coordinator_registered(&card));

    return create_client<CoordScheduler1>(
        server_address_to_sockaddr(card.address_for_sched));
  }

  static x23::Async<void> schedulercoord_null() { co_return; }

  x23::Async<CoordinatorRegistrationOutcome>
  schedulercoord_intro_coord(CoordCard card)
  {
    if (strcmp(card.protocol.c_str(), version::kHogdProtocol) != 0) {
      co_return CoordinatorRegistrationOutcome::
          CoordinatorRegistrationInvalidProtocol;
    }
    Registration::registration().register_coordinator(card);
    finish_init();

    co_return CoordinatorRegistrationOutcome::CoordinatorRegistrationOK;
  }

  // Submit an unvetted tx.
  x23::Async<void> schedulercoord_submit_unvetted(SignedTransactionBatch batch);

  // Submit a vetted tx.
  x23::Async<void> schedulercoord_submit_vetted(ValidatorID validator_id,
                                                SignedTransactionBatch batch);

  // Have the mempool put together a block of transactions and nominate the
  // block. [db_tip] specifies the last committed block.
  x23::Async<SchedulerNominationResult> schedulercoord_nominate(DbTip db_tip);

  // Have the mempool validate a block. [nomination] holds the block to
  // validate. [db_tip] specifies the last committed block.
  x23::Async<SchedulerValidationResult>
  schedulercoord_validate(DbTip db_tip, Nomination nomination);

  static x23::Async<void> schedulercoord_commit_block(BlockID block_id)
  {
    // TODO(jhumphri): Implement.
    co_return;
  }

  // These are just stubs that pass through the calls from the coordinator to
  // hogdb.
  x23::Async<DbTipResult> schedcoord_get_tip();

  // It is an error to shut down the mempool while there are transactions that
  // are still running.
  x23::Async<void> schedulercoord_shutdown();

  x23::Async<DbSchedResult>
  schedcoord_load_genesis_block(const GenesisBlock &genesis_block)
  {
    return db_sched_client_->dbsched_load_genesis_block(genesis_block);
  }

  static SHA256Hash
  get_block_hash(const x23::Vec<SignedTransactionHash> &hashes,
                 const BlockHash &prev_block_hash);

  bool ready() const { return ready_.load(std::memory_order_acquire); }

  void dump_stats() { scheduler_->dump_runqueue(); }

  void shut_down()
  {
    // TODO(jhumphri): Implement.
    shutdown_flag_ = true;
  }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  const std::function<x23::Async<bool>(const SignedTransaction &tx)>
      auth_filter_ = [&](const SignedTransaction &tx) -> x23::Async<bool> {
    co_return co_await authentication_->authenticate(tx);
  };

  // TODO(jhumphri): We are using this function to generate unique block IDs.
  // This will need to be reworked if/when we have multiple threads processing
  // blocks.
  static uint64_t get_ns_since_epoch()
  {
    auto now = std::chrono::system_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch());
    return ns.count();
  }

  void finish_init()
  {
    coord_client_ = std::make_unique<coord_sched_client_t>(get_client());
    // TODO(jhumphri): I think I just need to create a client for one of the
    // shards? Will the account information that the scheduler needs be
    // distributed across shards?
    db_sched_client_ = std::make_shared<db_sched_client_t>(
        std::move(MempoolDbRpc::get_clients()[0]));
    for (size_t i = 0; i < kNumClientsPerExecutor; i++) {
      for (auto &client : MempoolExecutorRpc::get_clients()) {
        exec_clients_.push_back(client);
      }
    }
    info("exec_clients_.size() = {}", exec_clients_.size());
    event_sched_client_ =
        std::make_unique<event_sched_client_t>(MempoolEventRpc::get_client());

    // TODO(jhumphri): Ask the db for the current block ID and fill in
    // [current_block_id_] with it.
    authentication_ =
        std::make_unique<Authentication>(current_block_id_, db_sched_client_);
    scheduler_ = std::make_shared<NewScheduler>(db_sched_client_);
    db_rpc_->register_scheduler(scheduler_);

    ready_.store(true, std::memory_order_release);
  }

  x23::Async<bool> create_snapshot(BlockID prev_block_id, BlockID block_id,
                                   BlockExecutionMode /*mode*/);
  x23::Async<void> abandon_snapshot(BlockID block_id);
  x23::Async<void> close_snapshot(BlockID block_id);
  x23::Async<void> commit_snapshot(BlockID block_id);
  x23::Async<void> finish_block(BlockID block_id, size_t executor_idx);

  x23::Detached
  propose_batch(BlockID block_id, x23::span<const SignedTransaction> txs,
                x23::XdrVec<NominatedTransaction> &transaction_nominations,
                x23::span<bool> ret_chunk, size_t executor_idx,
                x23::CoroCB<void> cb);

  x23::Detached propose_small_chunks_from_large_chunk(
      BlockID block_id, x23::span<const SignedTransaction> txs,
      x23::Vec<x23::XdrVec<NominatedTransaction>> &nomination_chunks,
      x23::span<bool> ret, x23::CoroCB<void> cb);

  x23::Detached propose_batch_in_chunks(
      BlockID block_id, x23::span<SignedTransaction> txs,
      x23::XdrVec<NominatedTransaction> &transaction_nominations,
      x23::span<bool> ret, x23::CoroCB<void> cb);

  x23::Async<ExecResult> open_proposal_block(BlockID block_id,
                                             size_t executor_idx);

  x23::Async<bool> validate_batch(BlockID block_id,
                                  const x23::Vec<ProposedTransaction> &txs,
                                  size_t executor_idx);

  x23::Async<ExecResult> open_validation_block(BlockID block_id,
                                               size_t executor_idx);

  x23::Async<void> mark_block_committed(BlockID block_id);

  std::shared_ptr<MempoolDbRpc> db_rpc_;

  std::unique_ptr<coord_sched_client_t> coord_client_;
  std::shared_ptr<db_sched_client_t> db_sched_client_;
  x23::Vec<exec_sched_client_t> exec_clients_;
  std::unique_ptr<event_sched_client_t> event_sched_client_;

  // TODO(jhumphri): Make the mempool work with multiple threads.
  BlockID current_block_id_ = 0;
  std::shared_ptr<NewScheduler> scheduler_;
  std::unique_ptr<Authentication> authentication_;

  std::atomic<bool> ready_ = false;
  std::atomic<bool> shutdown_flag_ = false;
};
static_assert(is_SchedulerCoord1<MempoolCoordinatorRpc *, x23::Async>);

} // namespace hogd
