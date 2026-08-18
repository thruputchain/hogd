#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
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
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_mempool/registration.h"
#include "hogd/hog_mempool/rpc_from_auth.h"
#include "hogd/hog_mempool/rpc_from_db.h"
#include "hogd/hog_mempool/rpc_from_event.h"
#include "hogd/hog_mempool/rpc_from_exec.h"
#include "hogd/hog_mempool/sharded_db_sched.h"
#include "hogd/hog_mempool/shared.h"
#include "hogd/metrics/health.h"
#include "hogd/metrics/metrics.h"
#include "hogd/version.h"
#include "parallel.hh"
#include "proposal.hh"
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
  static constexpr size_t kDefaultMempoolTxSizeLimit = 50'000'000;

  static constexpr size_t kNumSchedulerIterations = 25;
  static_assert(kMaxBlockSize % kNumSchedulerIterations == 0);
  static constexpr size_t kNumTxsPerNominationChunk =
      kMaxBlockSize / kNumSchedulerIterations;

  static constexpr size_t kChunkSize = 10;
  static constexpr size_t kNumClientsPerExecutor = 10;

  static constexpr size_t kFailedAuthenticationTxsBlockExpiryDelta = 5;

  MempoolCoordinatorRpc(MempoolDbRpc *db_rpc, MempoolAuthRpc *auth_rpc,
                        NetworkHash network_hash, size_t mempool_tx_size_limit,
                        size_t num_exec_shards = 1)
    : db_rpc_(db_rpc), auth_rpc_(auth_rpc),
      network_hash_(std::move(network_hash)),
      mempool_tx_size_limit_(static_cast<ssize_t>(mempool_tx_size_limit)),
      num_exec_shards_(num_exec_shards)
  {
    Health::instance().set_status(Health::Status::kStarting)();
  }

  ~MempoolCoordinatorRpc()
  {
    // Wait for coroutines that outlive their initiating RPC handler and still
    // touch our members -- the detached vetted-notify send and the
    // [auth_data_changed] callback (whose [auth_data_height_] broadcast tail
    // runs on a thread-pool worker) -- before member destructors tear that
    // state down.
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

  x23::Async<void> finish_runtime_init()
  {
    if (ready()) {
      co_return;
    }
    // Both waits suspend rather than block a pool worker; see [Registration].
    while (!initialized_.load(std::memory_order_acquire)) {
      co_await initialized_waiters_.wait(false);
    }
    std::ignore = co_await MempoolEventRpc::wait_for_registration();
    event_sched_client_ = std::make_unique<event_sched_client_t>(
        co_await MempoolEventRpc::get_client());
    for (size_t i = 0; i < kNumClientsPerExecutor; ++i) {
      auto clients = co_await MempoolExecutorRpc::get_clients(num_exec_shards_);
      for (auto &client : clients) {
        exec_clients_.push_back(std::move(client));
      }
    }
    co_await Health::instance().set_status(Health::Status::kOk);
    ready_.store(true, std::memory_order_release);
    ready_waiters_.store(true);
    ready_waiters_.broadcast(&default_threadpool());
  }

  static x23::Async<coord_sched_client_t> get_client()
  {
    CoordCard card;
    CHECK(Registration::registration().coordinator_registered(&card));

    auto ret = co_await create_client<CoordScheduler1>(
        server_address_to_sockaddr(card.address_for_sched),
        /*retry=*/false,
        /*max_message_size=*/kBulkRpcMaxMessageSize);
    if (!ret)
      panic("rpc_from_coord get_client connect failed: {}",
            ret.error().error_code().message());
    co_return std::move(*ret);
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
    CHECK(initialized_.load(std::memory_order_acquire));

    co_return CoordinatorRegistrationOutcome::CoordinatorRegistrationOK;
  }

  // Submit an unvetted tx.
  x23::Async<void>
  schedulercoord_submit_unvetted(ParallelSignedTransactionBatch batch);

  // Submit a vetted tx.
  x23::Async<void>
  schedulercoord_submit_vetted(ValidatorID validator_id,
                               ParallelSignedTransactionBatch batch);

  x23::Async<SchedulerProposalResult>
  schedulercoord_propose_block(BlockID parent_id, ProposalContext context);

  x23::Async<SchedulerValidationResult>
  schedulercoord_validate_block(BlockID parent_id, Proposal proposal);

  x23::Async<SchedulerCommitOutcome>
  schedulercoord_commit_block(BlockID block_id, ConsensusValue value);

  x23::Async<GenesisCeremonyResult>
  schedulercoord_commit_genesis_ceremony(GenesisCeremonyRequest request);

  // Returns the db tip of the last committed block.
  x23::Async<SchedulerGetTipResult> schedcoord_get_tip()
  {
    CHECK(initialized_.load(std::memory_order_acquire));

    // [last_committed_block_] is updated by [schedulercoord_commit_block()]
    // under the [commit_lock_] lock.
    x23::CoroGuard cg;
    co_await cg(commit_lock_);

    if (last_committed_block_.value.proposal_header.block_height == -1) {
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
    if (!ready()) {
      co_return;
    }

    // TODO(jhumphri): Implement.
    co_return;
  }

  x23::Async<DbSchedResult> schedcoord_load_genesis_configuration(
      GenesisConfiguration genesis_configuration)
  {
    CHECK(initialized_.load(std::memory_order_acquire));

    x23::CoroGuard commit_cg;
    co_await commit_cg(commit_lock_);

    CHECK_GE(last_committed_block_.value.proposal_header.block_height, -1);
    if (last_committed_block_.value.proposal_header.block_height != -1) {
      // The genesis block has already been loaded.
      co_return DbSchedResult::DbSchedResultError;
    }
    CHECK_EQ(last_committed_block_.value.proposal_header.block_height, -1);
    CHECK_EQ(auth_data_height(), -1);
    CHECK_EQ(scheduler_->transfer_finished_.load(), -1);

    ConsensusValue genesis_value{};
    genesis_value.proposal_header.block_height = 0;
    genesis_value.proposal_header.network_hash = network_hash_;
    genesis_value.proposal_header.genesis_configuration_hash =
        sha256(genesis_configuration);
    // Load genesis on every shard (fan-out lives in ShardedDbSched); the
    // cross-shard readiness barrier is ours to coordinate.
    {
      DbSchedResult ret =
          co_await sharded_db_sched_client_->load_genesis(genesis_configuration,
                                                          genesis_value);
      if (ret != DbSchedResult::DbSchedResultOK) {
        // Genesis failed on a shard: latch the failure and open the barrier so
        // a waiting [handle_transfer] drops its transfer instead of hanging.
        // The caller aborts the run.
        error("genesis load failed on a db shard ({}); aborting genesis", ret);
        scheduler_->genesis_load_failed_.store(true, std::memory_order_release);
        scheduler_->all_shards_genesis_ready_.store(true);
        scheduler_->all_shards_genesis_ready_.broadcast(&default_threadpool());
        co_return ret;
      }
    }

    DbTipResult db_res = co_await sharded_db_sched_client_->get_tip();
    CHECK_EQ(db_res.result(), DbSchedResult::DbSchedResultOK);
    last_committed_block_ = db_res.db_tip();
    CHECK_EQ(last_committed_block_.value.proposal_header.block_height, 0);
    CHECK_EQ(last_committed_block_.id, 0UL);
    Registration::registration().publish_event_canonical_tip(
        EventCanonicalTip{.block_id = last_committed_block_.id,
                          .consensus_value = last_committed_block_.value});
    scheduler_->set_current_block(
        last_committed_block_.value.proposal_header.block_height);
    co_await Health::instance().set_current_block_height(
        last_committed_block_.value.proposal_header.block_height);
    MempoolMetrics::instance().current_block_height.record(
        last_committed_block_.value.proposal_header.block_height);
    MempoolMetrics::instance().scheduler_pending_tx_count.record(
        scheduler_->size());
    MempoolMetrics::instance().scheduler_tx_cache_size.record(
        scheduler_->tx_cache_size());
    // Every shard loaded genesis OK: open the barrier so hog transfer deltas
    // can be processed (every shard can now serve the sharded hog-age reads).
    scheduler_->all_shards_genesis_ready_.store(true);
    scheduler_->all_shards_genesis_ready_.broadcast(&default_threadpool());

    // Then wait for those deltas to land, rather than seeding
    // [transfer_finished_] ourselves. Returning early reports genesis as loaded
    // while the scheduler still has no hogs for the genesis accounts, so a
    // caller that submits as soon as this returns -- hog-devnode's
    // initialization batch, chuckd's init batch -- has its txs discarded for
    // "all hogs available 0". Waiting cannot hang on a shard that owns none of
    // genesis: hogdb dispatches a HogsTransferred for every committed block
    // even when it has no deltas (see BackgroundCommitWorker::commit_job).
    co_await wait_for_transfer_data(
        last_committed_block_.value.proposal_header.block_height);

    co_return DbSchedResult::DbSchedResultOK;
  }

  x23::Async<DbSchedResult> schedcoord_load_genesis_configuration_v2(
      GenesisConfigurationV2 genesis_configuration)
  {
    CHECK(initialized_.load(std::memory_order_acquire));

    DbSchedResult ret;
    {
      x23::CoroGuard commit_cg;
      co_await commit_cg(commit_lock_);

      if (last_committed_block_.value.proposal_header.block_height != -1 ||
          genesis_configuration.network_hash != network_hash_) {
        co_return DbSchedResult::DbSchedResultError;
      }
      CHECK_EQ(auth_data_height(), -1);
      CHECK_EQ(scheduler_->transfer_finished_.load(), -1);

      ConsensusValue genesis_value{};
      genesis_value.proposal_header.block_height = 0;
      genesis_value.proposal_header.network_hash = network_hash_;
      genesis_value.proposal_header.genesis_configuration_hash =
          sha256(genesis_configuration);
      ret = co_await db_sched_client_->dbsched_load_genesis_configuration_v2(
          genesis_configuration, genesis_value);
      if (ret != DbSchedResult::DbSchedResultOK) {
        co_return ret;
      }

      DbTipResult db_res = co_await db_sched_client_->dbsched_get_tip();
      CHECK_EQ(db_res.result(), DbSchedResult::DbSchedResultOK);
      last_committed_block_ = db_res.db_tip();
      CHECK_EQ(last_committed_block_.value.proposal_header.block_height, 0);
      CHECK_EQ(last_committed_block_.id, 0UL);
      Registration::registration().publish_event_canonical_tip(
          EventCanonicalTip{.block_id = last_committed_block_.id,
                            .consensus_value = last_committed_block_.value});
      scheduler_->set_current_block(0);
      co_await Health::instance().set_current_block_height(0);
      MempoolMetrics::instance().current_block_height.record(0);
      MempoolMetrics::instance().scheduler_pending_tx_count.record(
          scheduler_->size());
      MempoolMetrics::instance().scheduler_tx_cache_size.record(
          scheduler_->tx_cache_size());
      scheduler_->transfer_finished_.store(0);
    }

    // The database's height-0 notification reaches us through the auth server,
    // whose callback also takes commit_lock_.  Release that lock explicitly
    // before waiting, and do not acknowledge the genesis load until the
    // downstream auth state is usable by the ceremony RPC.
    co_await wait_for_auth_data(/*expected_height=*/0);
    co_return ret;
  }

  x23::Async<x23::XdrVec<x23::Ptr<SignedTransaction>>>
  schedcoord_lookup_transactions(const TransactionHashBatch &tx_hashes)
  {
    x23::XdrVec<x23::Ptr<SignedTransaction>> ret;
    if (!ready()) {
      ret.resize(tx_hashes.size());
      co_return ret;
    }
    ret.reserve(tx_hashes.size());
    for (const TransactionHash &tx_hash : tx_hashes) {
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

  static SHA256Hash get_txset_hash(const x23::XdrVec<TransactionHash> &hashes);

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

  void set_finish_executor_started_test_hook(
      std::function<void(BlockID, size_t)> hook)
  {
    finish_executor_started_test_hook_ = std::move(hook);
  }

  void set_propose_batch_result_test_hook(
      std::function<void(ExecProposeBatchResult &)> hook)
  {
    propose_batch_result_test_hook_ = std::move(hook);
  }

  x23::Async<void> test_finish_all_executors(BlockID block_id)
  {
    co_await finish_all_executors(block_id);
  }

private:
  // clang-tidy complains that an exception may be thrown in the function
  // [operator=].
  // NOLINTNEXTLINE(bugprone-exception-escape)
  struct PendingBlock {
    DbTip db_tip;
    BlockID block_id;
    x23::XdrVec<SignedTransaction> txs;
    x23::XdrVec<TransactionHash> tx_hashes;

    x23::XdrVec<TransactionHash> result_tx_hashes;
    x23::XdrVec<NondeterministicProposalData> result_tx_nominations;
    SchedulerProposalResult proposal_result;

    x23::Vec<bool> statuses = x23::Vec<bool>(kMaxBlockSize, false);
    x23::Vec<x23::XdrVec<NondeterministicProposalData>>
        transaction_nomination_chunks;
    x23::XdrVec<TransactionHash> failed_tx_hashes;
  };
  struct JointCommitResult {
    DbBlockResult db;
    RPCError event_commit = RPCError::kRPCUnrecoverable;
    RPCError event_confirmation = RPCError::kRPCUnrecoverable;
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
    x23::XdrVec<TransactionHash> missing_tx_hashes;
  };

  struct PreparedAndValidatedChunk {
    x23::XdrVec<SignedTransaction> txs;
    x23::XdrVec<TransactionHash> missing_tx_hashes;
    bool validated = true;
  };

  x23::Async<void> finish_init()
  {
    coord_client_ =
        std::make_unique<coord_sched_client_t>(co_await get_client());
    {
      // The facade connects and binds clients by shard_id (fail-stop on a bad
      // set; see ShardedDbSched::connect).
      x23::XdrVec<ShardCard> cards;
      Registration::registration().shards_registered(&cards);
      sharded_db_sched_client_ =
          co_await ShardedDbSched::connect(std::move(cards));
    }
    {
      DbTipResult db_res = co_await sharded_db_sched_client_->get_tip();
      if (db_res.result() == DbSchedResult::DbSchedResultOK) {
        last_committed_block_ = db_res.db_tip();
        CHECK_GE(last_committed_block_.value.proposal_header.block_height, 0);
        auth_data_height_.store(
            last_committed_block_.value.proposal_header.block_height);
      }
      else {
        // [dbsched_get_tip()] returned an error because no blocks have been
        // committed yet (including the genesis block).
        ConsensusValue value{};
        value.proposal_header.block_height = -1;
        last_committed_block_ = DbTip{.id = std::numeric_limits<BlockID>::max(),
                                      .storage_long_hash = {},
                                      .value = std::move(value)};
        auth_data_height_.store(-1);
      }
    }
    info("finish_init(), system starting with db tip: height = {}, id = {}, "
         "block_hash = {}, events_hash = {}, db_hash = {}",
         last_committed_block_.value.proposal_header.block_height,
         last_committed_block_.id,
         show_byte_array(last_committed_block_.value.proposal_header.txs_hash),
         show_byte_array(
             last_committed_block_.value.proposal_header.events_hash),
         show_byte_array(last_committed_block_.storage_long_hash));
    Registration::registration().publish_event_canonical_tip(
        EventCanonicalTip{.block_id = last_committed_block_.id,
                          .consensus_value = last_committed_block_.value});

    scheduler_ = std::make_shared<Scheduler>(
        /*current_block=*/last_committed_block_.value.proposal_header
            .block_height,
        sharded_db_sched_client_);
    co_await Health::instance().set_current_block_height(
        last_committed_block_.value.proposal_header.block_height);
    MempoolMetrics::instance().current_block_height.record(
        last_committed_block_.value.proposal_header.block_height);
    MempoolMetrics::instance().scheduler_pending_tx_count.record(
        scheduler_->size());
    MempoolMetrics::instance().scheduler_tx_cache_size.record(
        scheduler_->tx_cache_size());
    scheduler_->transfer_finished_.store(
        last_committed_block_.value.proposal_header.block_height);
    // On restart genesis is already committed and won't re-load, so open the
    // barrier here (else [handle_transfer] would wait forever for a fan-out
    // that never runs). A fresh start leaves it closed until genesis load
    // completes.
    if (last_committed_block_.value.proposal_header.block_height >= 0) {
      scheduler_->all_shards_genesis_ready_.store(true);
      scheduler_->all_shards_genesis_ready_.broadcast(&default_threadpool());
    }
    co_await db_rpc_->register_scheduler(scheduler_);
    auth_rpc_->register_callback(
        [this](BlockHeight height, KeyChangeBatch key_changes) {
          return auth_data_changed(height, std::move(key_changes));
        });

    auth_client_ = std::make_unique<auth_sched_client_t>(
        co_await MempoolAuthRpc::get_client());

    initialized_.store(true, std::memory_order_release);
    initialized_waiters_.store(true);
    initialized_waiters_.broadcast(&default_threadpool());
  }

  x23::Async<DbSchedResult> create_snapshot(BlockID prev_block_id,
                                            BlockID block_id,
                                            BlockExecutionMode mode);
  x23::Async<RPCError> eventsched_open_block(BlockID block_id);
  x23::Async<DbSchedResult> abandon_snapshot(BlockID block_id);
  x23::Async<RPCError> eventsched_commit_block(BlockID block_id,
                                               const ConsensusValue &value);
  x23::Async<RPCError>
  eventsched_confirm_committed_block(const ConsensusValue &value);
  x23::Async<RPCError> eventsched_abandon_block(BlockID block_id);
  x23::Async<DbBlockResult> close_snapshot(BlockID block_id);
  x23::Async<EventBlockResult> eventsched_close_block(BlockID block_id,
                                                      BlockHeight block_height,
                                                      const ProposalTxs &txs);
  x23::Async<DbBlockResult> commit_snapshot(BlockID block_id,
                                            const ConsensusValue &value);
  x23::Async<void> finish_block(BlockID block_id, size_t executor_idx);
  x23::Async<void> finish_all_executors(BlockID block_id);
  x23::Vec<x23::DagNode<void>>
  add_finish_all_executors_nodes(x23::Dag &dag, const PendingBlock &cache,
                                 const x23::DagDeps &deps,
                                 const x23::Vec<x23::DagCleanup> &cleanups);

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
    x23::DagCleanup nomination_cleanup;
    x23::Vec<x23::DagNode<void>> schedule_chunks;
    x23::Vec<x23::DagNode<void>> propose_chunks;
    x23::Vec<x23::DagNode<void>> txset_insert_chunks;
    x23::Vec<x23::DagNode<void>> serialize_chunks;
    std::shared_ptr<MerkleTriePrefix> txset_trie;
    x23::DagNode<void> summarize;
    x23::DagNode<void> resize_txs;
  };
  struct FinishNominationNodes {
    x23::Vec<x23::DagNode<void>> finish_executors;
    x23::DagNode<DbBlockResult> close_snapshot;
    x23::DagNode<EventBlockResult> close_event;
    x23::DagNode<SHA256Hash> txset_hash;
    x23::DagNode<void> build_header;
  };

  InitializeBlockNodes add_initialize_block_nodes(
      x23::Dag &dag, const PendingBlock &cache, const x23::DagDeps &deps,
      const x23::DagCleanup &snapshot_cleanup,
      const x23::DagCleanup &event_cleanup,
      const x23::Vec<x23::DagCleanup> &executor_cleanups);
  ScheduleNodes add_schedule_nodes(x23::Dag &dag, PendingBlock &cache,
                                   x23::DagDeps scheduling_deps,
                                   const x23::DagDeps &proposal_deps);
  FinishNominationNodes add_finish_nominating_block_nodes(
      x23::Dag &dag, const DbTip &db_tip, const ProposalContext &context,
      PendingBlock &cache, const x23::DagDeps &proposal_deps,
      const x23::DagDeps &proposal_txs_deps,
      const x23::Vec<x23::DagCleanup> &executor_cleanups,
      const std::shared_ptr<MerkleTriePrefix> &txset_trie,
      const x23::DagDeps &txset_hash_deps);
  x23::Async<void> abandon_block(const PendingBlock &cache);
  x23::Async<JointCommitResult> commit_block(const PendingBlock &cache);

  x23::Async<SchedulerCommitOutcome>
  schedulercoord_commit_block_impl(BlockID block_id,
                                   ConsensusValue expected_value);

  x23::Async<bool>
  validate_batch_chunk(BlockID block_id,
                       x23::span<const ProposedTransaction> txs,
                       size_t executor_idx);

  x23::Async<PreparedValidationChunk> prepare_validation_chunk(
      x23::span<const TransactionHash> tx_hashes,
      x23::span<const NondeterministicProposalData> tx_nominations);

  x23::Async<PreparedAndValidatedChunk> prepare_and_validate_chunk(
      BlockID block_id, x23::span<const TransactionHash> tx_hashes,
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
    const BlockHeight cutoff =
        last_committed_block_.value.proposal_header.block_height;
    for (BlockHeight height = cutoff;
         height > cutoff - static_cast<BlockHeight>(
                               kFailedAuthenticationTxsBlockExpiryDelta);
         height--) {
      failed_authentication_txs_.remove(height);
    }
  }

  void mark_tx_failed_authentication(const TransactionHash &tx_hash)
  {
    const BlockHeight expiry =
        last_committed_block_.value.proposal_header.block_height +
        kFailedAuthenticationTxsBlockExpiryDelta;
    auto [tx_hashes, tx_hashes_lock] = failed_authentication_txs_[expiry];
    // We do not check whether the insert succeeds. It is possible for the same
    // tx to be added multiple times for the same block height (two validators
    // could each call [schedulercoord_submit_unvetted()] with the same tx).
    tx_hashes.insert(tx_hash, 0);
  }

  bool tx_failed_authentication(const TransactionHash &tx_hash)
  {
    const BlockHeight min_unexpired_height =
        last_committed_block_.value.proposal_header.block_height + 1;
    const BlockHeight max_unexpired_height =
        last_committed_block_.value.proposal_header.block_height +
        kFailedAuthenticationTxsBlockExpiryDelta;
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
  const NetworkHash network_hash_;
  const ssize_t mempool_tx_size_limit_;
  const size_t num_exec_shards_;

  std::unique_ptr<coord_sched_client_t> coord_client_;
  // Sole handle to the sharded db (see ShardedDbSched); shared with the
  // Scheduler for its hog-age reads/writes.
  std::shared_ptr<ShardedDbSched> sharded_db_sched_client_;
  x23::Vec<exec_sched_client_t> exec_clients_;
  std::unique_ptr<event_sched_client_t> event_sched_client_;
  std::function<void(BlockID, size_t)> finish_executor_started_test_hook_;
  std::function<void(ExecProposeBatchResult &)> propose_batch_result_test_hook_;
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
      SynchronizedMap<TransactionHash, char, SipHash24Hasher>;
  SynchronizedMap<BlockHeight, FailedAuthenticationTxSet>
      failed_authentication_txs_;

  DbTip last_committed_block_{};
  x23::CoroFutex<int64_t> auth_data_height_{-1};

  std::atomic<ssize_t> pending_detached_coroutines_ = 0;

  std::atomic<size_t> num_coordinators_registered_ = 0;
  std::atomic<bool> ready_ = false;
  x23::CoroFutex<bool> ready_waiters_{false};
  std::atomic<bool> initialized_ = false;
  x23::CoroFutex<bool> initialized_waiters_{false};

  std::chrono::steady_clock::time_point last_commit_ =
      std::chrono::steady_clock::now();
};
static_assert(is_SchedulerCoord1<MempoolCoordinatorRpc *, x23::Async>);

} // namespace hogd
