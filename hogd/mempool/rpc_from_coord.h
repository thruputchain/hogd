#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>

#include "coord_sched.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "event_rpc.hh"
#include "exec_sched.hh"
#include "genesis.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/mempool/registration.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_from_event.h"
#include "hogd/mempool/rpc_from_exec.h"
#include "hogd/mempool/shared.h"
#include "hogd/version.h"
#include "nomination.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "scheduler.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

// The mempool accepts transactions from the coordinator along with block
// nomination and validation requests.
class MempoolCoordinatorRpc {
public:
  using rpc_service = SchedulerCoord1;

  // The maximum number of txs in a block.
  static constexpr size_t kMaxBlockSize = 1'000'000;
  static constexpr size_t kNumSchedulerIterations = 25;
  static_assert(kMaxBlockSize % kNumSchedulerIterations == 0);
  static constexpr size_t kNumTxsPerNominationChunk =
      kMaxBlockSize / kNumSchedulerIterations;

  static constexpr size_t kChunkSize = 10;
  static constexpr size_t kNumClientsPerExecutor = 10;

  MempoolCoordinatorRpc(MempoolDbRpc *db_rpc) : db_rpc_(db_rpc) {}

  ~MempoolCoordinatorRpc() { shut_down(); }

  // Disable copy and move.
  MempoolCoordinatorRpc(const MempoolCoordinatorRpc &) = delete;
  MempoolCoordinatorRpc &operator=(const MempoolCoordinatorRpc &) = delete;
  MempoolCoordinatorRpc(MempoolCoordinatorRpc &&) = delete;
  MempoolCoordinatorRpc &operator=(MempoolCoordinatorRpc &&) = delete;

  std::shared_ptr<Scheduler> scheduler() { return scheduler_; }

  static x23::Async<coord_sched_client_t> get_client()
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
    info("got {}, expected {}", card.protocol.c_str(), version::kHogdProtocol);
    if (strcmp(card.protocol.c_str(), version::kHogdProtocol) != 0) {
      co_return CoordinatorRegistrationOutcome::
          CoordinatorRegistrationInvalidProtocol;
    }
    Registration::registration().register_coordinator(card);
    co_await finish_init();

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

  // Commits the block with ID [block_id].
  x23::Async<SchedulerCommitOutcome>
  schedulercoord_commit_block(BlockID block_id);

  // Returns the db tip of the last committed block.
  x23::Async<DbTip> schedcoord_get_tip() { co_return last_committed_block_; }

  // It is an error to shut down the mempool while there are transactions that
  // are still running.
  x23::Async<void> schedulercoord_shutdown()
  {
    // TODO(jhumphri): Implement.
    co_return;
  }

  x23::Async<DbSchedResult> schedcoord_load_genesis_configuration(
      GenesisConfiguration genesis_configuration)
  {
    auto ret = co_await db_sched_client_->dbsched_load_genesis_configuration(
        genesis_configuration);
    last_committed_block_ = {.height = 1,
                             .hash = sha256(genesis_configuration),
                             .id = generate_block_id()};
    co_return ret;
  }

  x23::Async<x23::XdrVec<x23::BorrowedPtr<SignedTransaction>>>
  schedcoord_lookup_transactions(const SignedTransactionHashBatch &tx_hashes)
  {
    x23::XdrVec<x23::BorrowedPtr<SignedTransaction>> ret;
    ret.reserve(tx_hashes.size());
    for (const SignedTransactionHash &tx_hash : tx_hashes) {
      x23::BorrowedPtr<SignedTransaction> tx(scheduler_->find_tx(tx_hash));
      ret.push_back(tx);
    }

    // [OptSignedTransactionBatch] is an
    // [x23::Vec<x23::Ptr<SignedTransaction>>]. We want to use [BorrowedPtrs],
    // which are not the same as [Ptrs], but they serialize the same way. So
    // we return a span of [BorrowedPtr]s.
    co_return ret;
  }

  // Generates a new block ID. This block ID is guaranteed to be unique for
  // the lifetime of this process. This function is thread-safe.
  static uint64_t generate_block_id()
  {
    static std::atomic<uint64_t> last_ns = 0;
    while (true) {
      auto now = std::chrono::system_clock::now();
      uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        now.time_since_epoch())
                        .count();
      uint64_t last_ns_relaxed = last_ns.load(std::memory_order_relaxed);
      if (ns == last_ns_relaxed) {
        continue;
      }
      if (last_ns.compare_exchange_weak(
              /*expected=*/last_ns_relaxed,
              /*desired=*/ns,
              /*order=*/std::memory_order_relaxed)) {
        return ns;
      }
    }
  }

  static SHA256Hash
  get_block_hash(const x23::XdrVec<SignedTransactionHash> &hashes,
                 const BlockHash &prev_block_hash);

  bool ready() const { return ready_.load(std::memory_order_acquire); }

  void dump_stats() { scheduler_->dump_runqueue(); }

  void shut_down()
  {
    size_t current_val;
    while ((current_val = pending_detached_coroutines_.load(
                std::memory_order_acquire)) != 0) {
      pending_detached_coroutines_.wait(/*old=*/current_val);
    }
  }

private:
  // clang-tidy complains that an exception may be thrown in the function
  // [operator=].
  // NOLINTNEXTLINE(bugprone-exception-escape)
  struct PendingBlock {
    DbTip db_tip;
    BlockID block_id;
    x23::XdrVec<SignedTransaction> txs;
    x23::XdrVec<SignedTransactionHash> tx_hashes;

    x23::Vec<SchedulerNominationChunk> sched_nominations =
        x23::Vec<SchedulerNominationChunk>(kNumSchedulerIterations, {});
    SchedulerNominationChunk last_chunk;

    x23::Vec<bool> statuses = x23::Vec<bool>(kMaxBlockSize, false);
    x23::Vec<x23::XdrVec<NominatedTransaction>> transaction_nomination_chunks;
    x23::XdrVec<SignedTransaction> failed_txs;
  };

  x23::Async<void> finish_init()
  {
    coord_client_ =
        std::make_unique<coord_sched_client_t>(co_await get_client());
    // TODO(jhumphri): I think I just need to create a client for one of the
    // shards? Will the account information that the scheduler needs be
    // distributed across shards?
    db_sched_client_ = std::make_shared<db_sched_client_t>(
        std::move((co_await MempoolDbRpc::get_clients())[0]));
    for (size_t i = 0; i < kNumClientsPerExecutor; i++) {
      auto clients = co_await MempoolExecutorRpc::get_clients();
      for (auto &client : clients) {
        exec_clients_.push_back(client);
      }
    }
    event_sched_client_ = std::make_unique<event_sched_client_t>(
        co_await MempoolEventRpc::get_client());

    // TODO(jhumphri): Ask the db for the current block ID and fill in
    // [current_block_id_] with it in the scheduler.
    scheduler_ = std::make_shared<Scheduler>(db_sched_client_);
    co_await db_rpc_->register_scheduler(scheduler_);

    ready_.store(true, std::memory_order_release);
  }

  x23::Async<DbSchedResult> create_snapshot(BlockID prev_block_id,
                                            BlockID block_id,
                                            BlockExecutionMode mode);
  x23::Async<RPCError> eventsched_open_block(BlockID block_id);
  x23::Async<DbSchedResult> abandon_snapshot(BlockID block_id);
  x23::Async<RPCError> eventsched_abandon_block(BlockID block_id);
  x23::Async<DbBlockResult> close_snapshot(BlockID block_id);
  x23::Async<EventBlockResult> eventsched_close_block(BlockID block_id);
  x23::Async<DbBlockResult> commit_snapshot(BlockID block_id);
  x23::Async<void> finish_block(BlockID block_id, size_t executor_idx);
  x23::Async<void> finish_all_executors(BlockID block_id);

  x23::Async<void>
  propose_batch(BlockID block_id, x23::span<const SignedTransaction> txs,
                x23::XdrVec<NominatedTransaction> &transaction_nominations,
                x23::span<bool> ret_chunk, size_t executor_idx);

  x23::Async<void> multiplex_large_chunk_across_executors(
      BlockID block_id, x23::span<const SignedTransaction> txs,
      x23::Vec<x23::XdrVec<NominatedTransaction>> &nomination_chunks,
      x23::span<bool> ret);

  x23::Async<void> propose_batch_in_chunks(
      BlockID block_id, x23::span<SignedTransaction> txs,
      x23::XdrVec<NominatedTransaction> &transaction_nominations,
      x23::span<bool> ret);

  x23::Async<ExecResult> open_proposal_block(BlockID block_id,
                                             size_t executor_idx);

  x23::Async<bool> initialize_block(const PendingBlock &cache);
  x23::Async<void> schedule(PendingBlock &cache);
  static void process_tx_outcomes(PendingBlock &cache);
  x23::Async<void> send_nomination_chunk(const SchedulerNominationChunk &chunk);
  x23::Detached finish_nominating_block(PendingBlock &cache);
  x23::Async<void> abandon_block(const PendingBlock &cache);
  x23::Async<void> commit_block(const PendingBlock &cache);

  x23::Async<bool> validate_batch(BlockID block_id,
                                  const x23::Vec<ProposedTransaction> &txs,
                                  size_t executor_idx);

  x23::Async<ExecResult> open_validation_block(BlockID block_id,
                                               size_t executor_idx);

  x23::Async<void> increment_num_validations_in_progress()
  {
    x23::CoroGuard cg;
    co_await cg(validated_block_cache_lock_);
    num_validations_in_progress_++;
  }
  x23::Async<void> decrement_num_validations_in_progress()
  {
    x23::CoroGuard cg;
    co_await cg(validated_block_cache_lock_);
    CHECK_GT(num_validations_in_progress_, 0UL);
    num_validations_in_progress_--;
  }

  MempoolDbRpc *db_rpc_;

  std::unique_ptr<coord_sched_client_t> coord_client_;
  std::shared_ptr<db_sched_client_t> db_sched_client_;
  x23::Vec<exec_sched_client_t> exec_clients_;
  std::unique_ptr<event_sched_client_t> event_sched_client_;

  // TODO(jhumphri): Make the mempool work with multiple threads.
  std::shared_ptr<Scheduler> scheduler_;

  // Ensures only one block is nominated at a time.
  x23::CoroLock nominated_block_cache_lock_;
  // Protects [validated_block_cache_].
  x23::CoroLock validated_block_cache_lock_;
  size_t num_validations_in_progress_ = 0;
  // Ensures that a commit does not occur while a block is being nominated or
  // validated, and vice versa.
  // This lock is acquired in shared mode when a block is being nominated or
  // validated. This lock is acquired in exclusive mode when a block is being
  // committed.
  PendingBlock nominated_block_cache_ = {};
  std::unordered_map<BlockID, PendingBlock> validated_block_cache_;
  DbTip last_committed_block_{};
  std::atomic<size_t> pending_detached_coroutines_ = 0;

  std::atomic<bool> ready_ = false;
};
static_assert(is_SchedulerCoord1<MempoolCoordinatorRpc *, x23::Async>);

} // namespace hogd
