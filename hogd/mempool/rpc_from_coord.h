#pragma once

#include <atomic>
#include <chrono>
#include <utility>

#include "coord_sched.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "exec_sched.hh"
#include "genesis.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/mempool/authentication.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_from_event.h"
#include "hogd/mempool/rpc_from_exec.h"
#include "hogd/version.h"
#include "nomination.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "scheduler.h"
#include "transaction.hh"
#include "types.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/rpc.h"
#include "x23/vec.h"

namespace hogd {

// The mempool accepts transactions from the coordinator along with block
// nomination and validation requests.
class MempoolCoordinatorRpc {
public:
  using rpc_service = SchedulerCoord1;

  // The maximum number of txs in a block.
  static constexpr size_t kMaxBlockSize = 128;

  MempoolCoordinatorRpc() = default;

  // Disable copy and move.
  MempoolCoordinatorRpc(const MempoolCoordinatorRpc &) = delete;
  MempoolCoordinatorRpc &operator=(const MempoolCoordinatorRpc &) = delete;
  MempoolCoordinatorRpc(MempoolCoordinatorRpc &&) = delete;
  MempoolCoordinatorRpc &operator=(MempoolCoordinatorRpc &&) = delete;

  Scheduler &scheduler() { return *scheduler_; }
  Authentication &authentication() { return *authentication_; }

  static coord_sched_client_t get_client(x23::CoroPoll &cp)
  {
    CoordCard card;
    CHECK(Registration::registration().coordinator_registered(&card));

    x23::UniqueFd fd =
        x23::tcpconnect(server_address_to_sockaddr(card.address));
    return x23::rpc_stream_logged(cp, fd.release())->client(CoordScheduler1{});
  }

  static x23::Async<void> schedulercoord_null() { co_return; }

  x23::Async<CoordinatorRegistrationOutcome>
  schedulercoord_intro_coord(CoordCard card)
  {
    if (card.protocol.c_str() != version::kHogdProtocol) {
      co_return CoordinatorRegistrationOutcome::
          CoordinatorRegistrationInvalidProtocol;
    }
    current_block_id_ = card.current_block_id;
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

  // These are just stubs that pass through the calls from the coordinator to
  // hogdb.
  x23::Async<DbTipResult> schedcoord_get_tip();
  x23::Async<DbSchedResult>
  schedcoord_load_genesis_block(const GenesisBlock &genesis_block);

  // It is an error to shut down the mempool while there are transactions that
  // are still running.
  x23::Async<void> schedulercoord_shutdown();

  void shut_down()
  {
    // TODO(jhumphri): Implement.
    shutdown_flag_ = true;
  }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  // To compute the block hash, we insert tx hashes into a Merkle Trie. We make
  // insertion faster with multiple threads. Using a lot of threads could cause
  // worse performance if each thread has few hashes to insert. Thus, to
  // calculate a reasonable number of threads to use, we ensure that each thread
  // has at least [kMinHashesPerThread] hashes to insert.
  static constexpr size_t kMinHashesPerThread = 16'384;

  const std::function<bool(const SignedTransaction &tx)> auth_filter_ =
      [&](const SignedTransaction &tx) {
        return authentication_->authenticate(tx);
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
    coord_client_ = std::make_unique<coord_sched_client_t>(
        std::move(get_client(client_cp_)));
    // TODO(jhumphri): I think I just need to create a client for one of the
    // shards? Will the account information that the scheduler needs be
    // distributed across shards?
    db_sched_client_ = std::make_unique<db_sched_client_t>(
        std::move(MempoolDbRpc::get_clients(client_cp_)[0]));
    exec_clients_ = MempoolExecutorRpc::get_clients(client_cp_);
    event_sched_client_ = std::make_unique<event_sched_client_t>(
        std::move(MempoolEventRpc::get_client(client_cp_)));

    authentication_ =
        std::make_unique<Authentication>(current_block_id_, *db_sched_client_);
    scheduler_ = std::make_unique<Scheduler>(*db_sched_client_);
  }

  x23::Async<bool> create_snapshot(BlockID prev_block_id, BlockID block_id,
                                   BlockExecutionMode /*mode*/);
  x23::Async<void> abandon_snapshot(BlockID block_id);
  x23::Async<void> close_snapshot(BlockID block_id);
  x23::Async<void> commit_snapshot(BlockID block_id);
  x23::Async<void> finish_block(BlockID block_id);

  static SHA256Hash
  get_block_hash(const x23::Vec<SignedTransactionHash> &hashes);

  x23::Async<x23::Vec<bool>>
  propose_batch(BlockID block_id, const x23::Vec<SignedTransaction> &txs,
                x23::XdrVec<NominatedTransaction> &transaction_nominations);

  x23::Async<bool> open_proposal_block(BlockID block_id);

  x23::Async<bool> validate_batch(BlockID block_id,
                                  const x23::Vec<ProposedTransaction> &txs);

  x23::Async<bool> open_validation_block(BlockID block_id);

  x23::Async<void> mark_block_committed(BlockID block_id);

  x23::ThreadPool client_pool_;
  x23::CoroPoll client_cp_ = x23::CoroPoll(client_pool_);
  std::unique_ptr<coord_sched_client_t> coord_client_;
  std::unique_ptr<db_sched_client_t> db_sched_client_;
  x23::Vec<exec_sched_client_t> exec_clients_;
  std::unique_ptr<event_sched_client_t> event_sched_client_;

  // TODO(jhumphri): Make the mempool work with multiple threads.
  BlockID current_block_id_;
  std::unique_ptr<Scheduler> scheduler_;
  std::unique_ptr<Authentication> authentication_;

  std::atomic<bool> shutdown_flag_ = false;
};
static_assert(is_SchedulerCoord1<MempoolCoordinatorRpc *, x23::Async>);

class MempoolCoordinatorServer : public RpcServer<MempoolCoordinatorRpc> {};

} // namespace hogd
