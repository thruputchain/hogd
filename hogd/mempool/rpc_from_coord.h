#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "coord_sched.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "event_rpc.hh"
#include "exec_result.hh"
#include "genesis.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/mempool/registration.h"
#include "hogd/mempool/rpc_from_auth.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_from_event.h"
#include "hogd/mempool/rpc_from_exec.h"
#include "hogd/mempool/shared.h"
#include "hogd/metrics/health.h"
#include "hogd/metrics/metrics.h"
#include "hogd/version.h"
#include "nomination.hh"
#include "parallel.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "scheduler.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/dag.h"
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
  static constexpr size_t kDefaultMempoolTxSizeLimit = 5'000'000;

  static constexpr size_t kNumSchedulerIterations = 25;
  static_assert(kMaxBlockSize % kNumSchedulerIterations == 0);
  static constexpr size_t kNumTxsPerNominationChunk =
      kMaxBlockSize / kNumSchedulerIterations;

  static constexpr size_t kChunkSize = 10;
  static constexpr size_t kNumClientsPerExecutor = 10;

  static constexpr size_t kFailedAuthenticationTxsBlockExpiryDelta = 5;

  MempoolCoordinatorRpc(MempoolDbRpc *db_rpc, MempoolAuthRpc *auth_rpc,
                        uint64_t network_id, size_t mempool_tx_size_limit)
    : db_rpc_(db_rpc), auth_rpc_(auth_rpc), network_id_(network_id),
      mempool_tx_size_limit_(static_cast<ssize_t>(mempool_tx_size_limit))
  {}

  ~MempoolCoordinatorRpc()
  {
    while (pending_detached_coroutines_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

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

    auto ret = create_client<CoordScheduler1>(
        server_address_to_sockaddr(card.address_for_sched),
        /*retry=*/false,
        /*max_message_size=*/LoadgenRpc::kRpcMaxMessageSize);
    return ret;
  }

  static x23::Async<void> schedulercoord_null() { co_return; }

  x23::Async<CoordinatorRegistrationOutcome>
  schedulercoord_intro_coord(CoordCard card)
  {
    info("got {}, expected {}", card.protocol.c_str(),
         version::kHogdProtocol());
    if (std::string_view(card.protocol) !=
        std::string_view(version::kHogdProtocol())) {
      co_return CoordinatorRegistrationOutcome::
          CoordinatorRegistrationInvalidProtocol;
    }

    // Only one coordinator is allowed to register with the mempool.
    size_t expected_coordinators_registered = 0;
    if (!num_coordinators_registered_.compare_exchange_strong(
            /*expected=*/expected_coordinators_registered, /*desired=*/1,
            /*order=*/std::memory_order_relaxed)) {
      co_return CoordinatorRegistrationOutcome::
          CoordinatorRegistrationAlreadyRegistered;
    }
    CHECK(!ready());

    Registration::registration().register_coordinator(card);
    co_await finish_init();
    CHECK(ready());

    co_return CoordinatorRegistrationOutcome::CoordinatorRegistrationOK;
  }

  // Submit an unvetted tx.
  x23::Async<void>
  schedulercoord_submit_unvetted(ParallelSignedTransactionBatch batch);

  // Submit a vetted tx.
  x23::Async<void>
  schedulercoord_submit_vetted(ValidatorID validator_id,
                               ParallelSignedTransactionBatch batch);

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
  x23::Async<SchedulerGetTipResult> schedcoord_get_tip()
  {
    CHECK(ready());

    // [last_committed_block_] is updated by [schedulercoord_commit_block()]
    // under the [commit_lock_] lock.
    x23::CoroGuard cg;
    co_await cg(commit_lock_);

    if (last_committed_block_.height == -1) {
      co_return SchedulerGetTipResult(
          SchedulerGetTipOutcome::SchedulerGetTipNoGenesisBlock);
    }

    SchedulerGetTipResult ret(SchedulerGetTipOutcome::SchedulerGetTipOK);
    ret.db_tip() = last_committed_block_;
    co_return ret;
  }

  // It is an error to shut down the mempool while there are transactions that
  // are still running.
  x23::Async<void> schedulercoord_shutdown() const
  {
    CHECK(ready());

    // TODO(jhumphri): Implement.
    co_return;
  }

  x23::Async<DbSchedResult> schedcoord_load_genesis_configuration(
      GenesisConfiguration genesis_configuration)
  {
    CHECK(ready());

    x23::CoroGuard commit_cg;
    co_await commit_cg(commit_lock_);

    CHECK_GE(last_committed_block_.height, -1);
    if (last_committed_block_.height != -1) {
      // The genesis block has already been loaded.
      co_return DbSchedResult::DbSchedResultError;
    }
    CHECK_EQ(last_committed_block_.height, -1);
    CHECK_EQ(auth_data_height(), -1);
    CHECK_EQ(scheduler_->transfer_finished_.load(), -1);

    MerkleTrie empty_trie;
    empty_trie.generate_all_hashes();
    BlockHash empty_trie_hash = empty_trie.get_root_hash();
    DbSchedResult ret =
        co_await db_sched_client_->dbsched_load_genesis_configuration(
            genesis_configuration, network_id_, empty_trie_hash,
            empty_trie_hash);
    if (ret != DbSchedResult::DbSchedResultOK) {
      co_return ret;
    }

    DbTipResult db_res = co_await db_sched_client_->dbsched_get_tip();
    CHECK_EQ(db_res.result(), DbSchedResult::DbSchedResultOK);
    last_committed_block_ = db_res.db_tip();
    CHECK_EQ(last_committed_block_.height, 0);
    CHECK_EQ(last_committed_block_.id, 0UL);
    scheduler_->set_current_block(last_committed_block_.height);
    co_await Health::instance().set_current_block_height(
        last_committed_block_.height);
    MempoolMetrics::instance().current_block_height.record(
        last_committed_block_.height);
    MempoolMetrics::instance().scheduler_pending_tx_count.record(
        scheduler_->size());
    MempoolMetrics::instance().scheduler_tx_cache_size.record(
        scheduler_->tx_cache_size());
    scheduler_->transfer_finished_.store(last_committed_block_.height);

    co_return ret;
  }

  x23::Async<x23::XdrVec<x23::Ptr<SignedTransaction>>>
  schedcoord_lookup_transactions(const SignedTransactionHashBatch &tx_hashes)
  {
    CHECK(ready());

    x23::XdrVec<x23::Ptr<SignedTransaction>> ret;
    ret.reserve(tx_hashes.size());
    for (const SignedTransactionHash &tx_hash : tx_hashes) {
      std::shared_ptr<const SignedTransaction> tx =
          scheduler_->find_tx(tx_hash);
      if (tx) {
        // This is a bit awkward, but I need to copy [tx] into an [x23::Ptr]
        // using [x23::Ptr]'s value constructor rather than just use the
        // existing [tx] pointer. This avoids the issue where [tx] is freed from
        // the scheduler by another thread before we can serialize and return
        // the tx from this RPC.
        //
        // Even if we hold [scheduler_->global_lock_] to protect the txs rather
        // than copy them into [tx_store], the lock is released at the end of
        // this function, which is *before* the txs are accessed and serialized
        // by x23. So that is still not safe.
        ret.push_back(x23::Ptr<SignedTransaction>(*tx));
      }
      else {
        ret.push_back(nullptr);
      }
    }

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
  get_txset_hash(const x23::XdrVec<SignedTransactionHash> &hashes,
                 bool log_timings = false);

  bool ready() const { return ready_.load(std::memory_order_acquire); }

  BlockHeight auth_data_height() { return auth_data_height_.load(); }

  x23::Async<void> auth_data_changed(BlockHeight height,
                                     KeyChangeBatch key_changes);

  x23::Async<void> wait_for_block_commit(BlockHeight expected_height)
  {
    int64_t current_height;
    // [last_committed_block_] is not an [x23::CoroFutex], so we cannot [wait()]
    // on it until a certain height is reached. However, [auth_data_height_] is
    // a safe substitute for this, plus [schedulercoord_validate()], which calls
    // this function, needs to wait for the latest auth data to arrive anyway.
    while ((current_height = auth_data_height_.load()) <
           static_cast<int64_t>(expected_height)) {
      co_await auth_data_height_.wait(current_height);
    }
    co_return;
  }

  x23::Async<void> wait_for_auth_data(BlockHeight expected_height)
  {
    int64_t current_height;
    while ((current_height = auth_data_height_.load()) !=
           static_cast<int64_t>(expected_height)) {
      CHECK_EQ(current_height + 1, expected_height);
      co_await auth_data_height_.wait(current_height);
    }
    co_return;
  }

  x23::Async<void> wait_for_transfer_data(BlockHeight expected_height)
  {
    int64_t current_height;
    while ((current_height = scheduler_->transfer_finished_.load()) !=
           static_cast<int64_t>(expected_height)) {
      CHECK_EQ(current_height + 1, expected_height);
      co_await scheduler_->transfer_finished_.wait(current_height);
    }
    co_return;
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

    x23::XdrVec<SignedTransactionHash> result_tx_hashes;
    x23::XdrVec<NondeterministicProposalData> result_tx_nominations;
    SchedulerNominationResult nomination_result;

    x23::Vec<bool> statuses = x23::Vec<bool>(kMaxBlockSize, false);
    x23::Vec<x23::XdrVec<NondeterministicProposalData>>
        transaction_nomination_chunks;
    x23::XdrVec<SignedTransactionHash> failed_tx_hashes;
  };

  struct NominationTelemetry {
    bool succeeded = false;
    double duration = 0.0;
    size_t scheduled_tx_count = 0;
    size_t nominated_tx_count = 0;
    size_t failed_tx_count = 0;
  };

  struct PreparedValidationChunk {
    x23::Vec<ProposedTransaction> proposed_txs;
    x23::XdrVec<SignedTransactionHash> missing_tx_hashes;
  };

  struct PreparedAndValidatedChunk {
    x23::XdrVec<SignedTransaction> txs;
    x23::XdrVec<SignedTransactionHash> missing_tx_hashes;
    bool validated = true;
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

    {
      DbTipResult db_res = co_await db_sched_client_->dbsched_get_tip();
      if (db_res.result() == DbSchedResult::DbSchedResultOK) {
        last_committed_block_ = db_res.db_tip();
        CHECK_GE(last_committed_block_.height, 0);
        auth_data_height_.store(last_committed_block_.height);
      }
      else {
        // [dbsched_get_tip()] returned an error because no blocks have been
        // committed yet (including the genesis block).
        last_committed_block_ = {.height = -1,
                                 .block_hash = {},
                                 .events_hash = {},
                                 .db_hash = {},
                                 .id = 0};
        auth_data_height_.store(-1);
      }
    }
    info("finish_init(), system starting with db tip: height = {}, id = {}, "
         "block_hash = {}, events_hash = {}, db_hash = {}",
         last_committed_block_.height, last_committed_block_.id,
         show_byte_array(last_committed_block_.block_hash),
         show_byte_array(last_committed_block_.events_hash),
         show_byte_array(last_committed_block_.db_hash));

    scheduler_ = std::make_shared<Scheduler>(
        /*current_block=*/last_committed_block_.height, db_sched_client_);
    co_await Health::instance().set_current_block_height(
        last_committed_block_.height);
    MempoolMetrics::instance().current_block_height.record(
        last_committed_block_.height);
    MempoolMetrics::instance().scheduler_pending_tx_count.record(
        scheduler_->size());
    MempoolMetrics::instance().scheduler_tx_cache_size.record(
        scheduler_->tx_cache_size());
    scheduler_->transfer_finished_.store(last_committed_block_.height);
    co_await db_rpc_->register_scheduler(scheduler_);
    auth_rpc_->register_callback(
        [this](BlockHeight height, KeyChangeBatch key_changes) {
          return auth_data_changed(height, std::move(key_changes));
        });

    auth_client_ = std::make_unique<auth_sched_client_t>(
        co_await MempoolAuthRpc::get_client());

    ready_.store(true, std::memory_order_release);
    co_await Health::instance().set_status(Health::Status::kOk);
  }

  x23::Async<DbSchedResult> create_snapshot(BlockID prev_block_id,
                                            BlockID block_id,
                                            BlockExecutionMode mode);
  x23::Async<RPCError> eventsched_open_block(BlockID block_id);
  x23::Async<DbSchedResult> abandon_snapshot(BlockID block_id);
  x23::Async<RPCError> eventsched_commit_block(BlockID block_id);
  x23::Async<RPCError> eventsched_abandon_block(BlockID block_id);
  x23::Async<DbBlockResult> close_snapshot(BlockID block_id);
  x23::Async<EventBlockResult> eventsched_close_block(BlockID block_id,
                                                      BlockHeight block_height);
  x23::Async<DbBlockResult> commit_snapshot(BlockID block_id,
                                            BlockHash block_hash,
                                            SHA256Hash events_hash);
  x23::Async<void> finish_block(BlockID block_id, size_t executor_idx);
  x23::Async<void> finish_all_executors(BlockID block_id);

  x23::Async<void> propose_batch(
      BlockID block_id, x23::span<const SignedTransaction> txs,
      x23::XdrVec<NondeterministicProposalData> &transaction_nominations,
      x23::span<bool> ret_chunk, size_t executor_idx);

  x23::Async<void> multiplex_large_chunk_across_executors(
      BlockID block_id, x23::span<const SignedTransaction> txs,
      x23::Vec<x23::XdrVec<NondeterministicProposalData>> &nomination_chunks,
      x23::span<bool> ret);

  x23::Async<void> propose_batch_in_chunks(
      BlockID block_id, x23::span<SignedTransaction> txs,
      x23::XdrVec<NondeterministicProposalData> &transaction_nominations,
      x23::span<bool> ret);

  x23::Async<ExecResult> open_proposal_block(BlockID block_id,
                                             size_t executor_idx);

  struct InitializeBlockNodes {
    x23::DagNode<DbSchedResult> create_snapshot;
    x23::DagNode<RPCError> open_event;
    x23::Vec<x23::DagNode<ExecResult>> open_proposal_blocks;
    x23::DagNode<bool> initialized;
  };
  struct ScheduleNodes {
    x23::DagNode<void> lock_scheduler;
    x23::Vec<x23::DagNode<void>> schedule_chunks;
    x23::Vec<x23::DagNode<void>> propose_chunks;
    x23::Vec<x23::DagNode<void>> txset_insert_chunks;
    x23::Vec<x23::DagNode<void>> serialize_chunks;
    std::shared_ptr<MerkleTriePrefix> txset_trie;
    x23::DagNode<void> summarize;
    x23::DagNode<void> resize_txs;
  };
  struct FinishNominationNodes {
    x23::DagNode<void> finish_executors;
    x23::DagNode<DbBlockResult> close_snapshot;
    x23::DagNode<EventBlockResult> close_event;
    x23::DagNode<SHA256Hash> txset_hash;
    x23::DagNode<void> serialize_nomination;
    x23::DagNode<void> build_header;
  };

  InitializeBlockNodes add_initialize_block_nodes(x23::Dag &dag,
                                                  const PendingBlock &cache,
                                                  const x23::DagDeps &deps);
  ScheduleNodes add_schedule_nodes(x23::Dag &dag, PendingBlock &cache,
                                   x23::DagDeps scheduling_deps,
                                   const x23::DagDeps &proposal_deps);
  FinishNominationNodes add_finish_nominating_block_nodes(
      x23::Dag &dag, const DbTip &db_tip, PendingBlock &cache,
      const x23::DagDeps &proposal_deps,
      const std::shared_ptr<MerkleTriePrefix> &txset_trie,
      const x23::DagDeps &txset_hash_deps, const x23::DagDeps &serialized_deps);
  x23::Async<void> abandon_block(const PendingBlock &cache);
  x23::Async<void> commit_block(const PendingBlock &cache);

  x23::Async<bool>
  validate_batch_chunk(BlockID block_id,
                       x23::span<const ProposedTransaction> txs,
                       size_t executor_idx);

  x23::Async<PreparedValidationChunk> prepare_validation_chunk(
      x23::span<const SignedTransactionHash> tx_hashes,
      x23::span<const NondeterministicProposalData> tx_nominations);

  x23::Async<PreparedAndValidatedChunk> prepare_and_validate_chunk(
      BlockID block_id, x23::span<const SignedTransactionHash> tx_hashes,
      x23::span<const NondeterministicProposalData> tx_nominations);

  x23::Async<bool> multiplex_large_validation_chunk_across_executors(
      BlockID block_id, x23::span<const ProposedTransaction> txs);

  x23::Async<bool> validate_batch(BlockID block_id,
                                  const x23::Vec<ProposedTransaction> &txs);

  x23::Async<ExecResult> open_validation_block(BlockID block_id,
                                               size_t executor_idx);

  static void record_submitted_vetted(size_t count);
  static void record_submitted_unvetted(size_t count);
  static void record_nomination_attempt(const NominationTelemetry &telemetry);
  static void record_validation_attempt(SchedulerValidationOutcome outcome,
                                        double duration, size_t tx_count);

  x23::Async<bool> increment_num_validations_in_progress()
  {
    bool admitted = false;
    num_validations_in_progress_.rmw([&admitted](int64_t &count) {
      if (count == -1) {
        return;
      }
      ++count;
      admitted = true;
    });
    if (admitted) {
      MempoolMetrics::instance().validation_in_progress_count.record(
          num_validations_in_progress_.load());
    }
    co_return admitted;
  }

  x23::Async<void> wait_for_validation_gate_open()
  {
    while (true) {
      int64_t current_validations = num_validations_in_progress_.load();
      CHECK_GE(current_validations, -1L);
      if (current_validations != -1) {
        co_return;
      }
      co_await num_validations_in_progress_.wait(current_validations);
    }
  }

  x23::Async<void> decrement_num_validations_in_progress()
  {
    num_validations_in_progress_.rmw([](int64_t &count) {
      CHECK_GT(count, 0L);
      --count;
    });
    MempoolMetrics::instance().validation_in_progress_count.record(
        num_validations_in_progress_.load());
    num_validations_in_progress_.broadcast(&default_threadpool());
    co_return;
  }

  x23::Async<void> close_validation_gate()
  {
    while (true) {
      int64_t current_validations = num_validations_in_progress_.load();
      CHECK_GE(current_validations, 0L);

      bool closed = false;
      num_validations_in_progress_.rmw([&closed](int64_t &count) {
        if (count == 0) {
          count = -1;
          closed = true;
        }
      });
      if (closed) {
        MempoolMetrics::instance().validation_in_progress_count.record(0);
        co_return;
      }
      co_await num_validations_in_progress_.wait(current_validations);
    }
  }

  void open_validation_gate()
  {
    num_validations_in_progress_.rmw([](int64_t &count) {
      CHECK_EQ(count, -1L);
      count = 0;
    });
    MempoolMetrics::instance().validation_in_progress_count.record(0);
    num_validations_in_progress_.broadcast(&default_threadpool());
  }

  void clear_expired_failed_authentication_txs()
  {
    // Entries are keyed by the last committed block height at which the failed
    // authentication should still affect validation.
    const BlockHeight cutoff = last_committed_block_.height;
    for (BlockHeight height = cutoff;
         height > cutoff - static_cast<BlockHeight>(
                               kFailedAuthenticationTxsBlockExpiryDelta);
         height--) {
      failed_authentication_txs_.remove(height);
    }
  }

  void mark_tx_failed_authentication(const SignedTransactionHash &tx_hash)
  {
    const BlockHeight expiry =
        last_committed_block_.height + kFailedAuthenticationTxsBlockExpiryDelta;
    auto [tx_hashes, tx_hashes_lock] = failed_authentication_txs_[expiry];
    // We do not check whether the insert succeeds. It is possible for the same
    // tx to be added multiple times for the same block height (two validators
    // could each call [schedulercoord_submit_unvetted()] with the same tx).
    tx_hashes.insert(tx_hash, 0);
  }

  bool tx_failed_authentication(const SignedTransactionHash &tx_hash)
  {
    const BlockHeight min_unexpired_height = last_committed_block_.height + 1;
    const BlockHeight max_unexpired_height =
        last_committed_block_.height + kFailedAuthenticationTxsBlockExpiryDelta;
    for (BlockHeight height = min_unexpired_height;
         height <= max_unexpired_height; height++) {
      auto [tx_hashes, tx_hashes_lock] =
          failed_authentication_txs_.find(height);
      if (tx_hashes == nullptr) {
        continue;
      }
      auto [entry, tx_hash_lock] = tx_hashes->find(tx_hash);
      if (entry != nullptr) {
        return true;
      }
    }
    return false;
  }

  MempoolDbRpc *db_rpc_;
  MempoolAuthRpc *auth_rpc_;
  const uint64_t network_id_;
  const ssize_t mempool_tx_size_limit_;

  std::unique_ptr<coord_sched_client_t> coord_client_;
  std::shared_ptr<db_sched_client_t> db_sched_client_;
  x23::Vec<exec_sched_client_t> exec_clients_;
  std::unique_ptr<event_sched_client_t> event_sched_client_;
  std::unique_ptr<auth_sched_client_t> auth_client_;

  std::shared_ptr<Scheduler> scheduler_;

  // Ensures only one block is nominated at a time.
  x23::CoroLock nominated_block_cache_lock_;
  // Protects accesses to [validated_block_cache_]. If multiple blocks are
  // validated concurrently, [validated_block_cache_] could be accessed in
  // parallel without this lock.
  x23::CoroLock validated_block_cache_lock_;
  // Ensures that a commit does not occur while txs are being verified. Txs
  // are verified against a certain block height, so if they race with a block
  // commit, we could add txs to the scheduler that are no longer verified as
  // of the last committed block.
  x23::CoroLock commit_lock_;
  // [num_validations_in_progress_] is a gate: values > 0 are active
  // validations, 0 means validation is open, and -1 means commit has closed
  // validation admission.
  x23::CoroFutex<int64_t> num_validations_in_progress_{0};
  PendingBlock nominated_block_cache_ = {};
  std::unordered_map<BlockID, PendingBlock> validated_block_cache_;

  using FailedAuthenticationTxSet =
      SynchronizedMap<SignedTransactionHash, char, ByteArrayHasher>;
  SynchronizedMap<BlockHeight, FailedAuthenticationTxSet>
      failed_authentication_txs_;

  DbTip last_committed_block_{};
  x23::CoroFutex<int64_t> auth_data_height_{-1};

  std::atomic<ssize_t> pending_detached_coroutines_ = 0;

  std::atomic<size_t> num_coordinators_registered_ = 0;
  std::atomic<bool> ready_ = false;

  std::chrono::steady_clock::time_point last_commit_ =
      std::chrono::steady_clock::now();
};
static_assert(is_SchedulerCoord1<MempoolCoordinatorRpc *, x23::Async>);

} // namespace hogd
