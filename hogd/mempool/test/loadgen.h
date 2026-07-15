#pragma once

#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "db_sched.hh"
#include "discovery.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/shared.h"
#include "hogd/mempool/test/coordinator.h"
#include "loadgen.hh"
#include "proposal.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/sockio.h"
#include "x23/vec.h"

namespace hogd {

// Helper class to manage a loadgen client.
class LoadgenClient {
public:
  LoadgenClient(const std::string &loadgen_address)
  {
    // Parse the loadgen address.
    x23::HostPort host_port;
    if (!host_port.parse(loadgen_address)) {
      panic("Invalid loadgen address: {}", loadgen_address);
    }

    // Connect to the loadgen server.
    client_ = std::make_unique<loadgen_client_t>(
        create_client<LoadgenInterfaceV1>(
            host_port, /*retry=*/true,
            /*max_message_size=*/LoadgenRpc::kRpcMaxMessageSize)()
            .value());
  }

  // TODO(jhumphri): Add flag to infra.h to disable existing setup for tests. We
  // do not want that setup when using loadgen.
  template<typename SchedCoordClient>
  DbTip init_experiment(SchedCoordClient &sched_coord_client,
                        CoordinatorSchedRpc & /*coord_sched_rpc*/,
                        uint32_t num_accounts)
  {
    // Initialize the experiment.
    ExperimentConfig config(
        ExperimentType::kExperimentTypeERC20BasicAuthPayments);
    config.erc20_payments().num_addresses = num_accounts;

    LoadgenResult result = client_->generate_experiment(config)();
    CHECK_EQ(result.error(), LoadgenRPCError::kLoadgenRPCErrorOK);
    context_id_ = result.success().context_id;

    // Pass the genesis configuration to the db.
    CHECK_EQ(sched_coord_client.schedcoord_load_genesis_configuration(
                 result.success().genesis_configuration)(),
             DbSchedResult::DbSchedResultOK);

    // We need to nominate and commit the initialization transactions. So first,
    // we start by adding them to the mempool with
    // [schedulercoord_submit_vetted()].
    const x23::XdrVec<InitializationBlock> &init_txs =
        result.success().initialization_txs;
    constexpr ValidatorID kValidatorId{};
    SchedulerGetTipResult tip_result =
        sched_coord_client.schedcoord_get_tip()();
    CHECK_EQ(tip_result.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    DbTip ret = tip_result.db_tip();
    for (const InitializationBlock &init_tx : init_txs) {
      sched_coord_client.schedulercoord_submit_vetted(kValidatorId,
                                                      t_to_parallel_batch(
                                                          init_tx)())();

      // Now propose and commit the initialization transactions.
      size_t num_nominated_txs = 0;
      while (num_nominated_txs < init_tx.size()) {
        auto res_propose = sched_coord_client.schedulercoord_propose_block(
            ret.id, ProposalContext{})();
        CHECK_EQ(res_propose.outcome(),
                 SchedulerProposalOutcome::SchedulerProposalOK);
        const SchedulerProposedBlock &proposed = res_propose.proposed_block();
        {
          SignedTransactionBatch proposed_txs;
          CHECK(parallel_batch_to_t(proposed.txs, proposed_txs)());
          num_nominated_txs += proposed_txs.size();
        }

        ConsensusValue value{};
        value.prev_hash = sha256(ret.value);
        value.proposal_header = proposed.proposal.header;
        value.proposal_txs_hash = sha256(proposed.proposal.transactions);
        CHECK_EQ(sched_coord_client.schedulercoord_commit_block(
                     proposed.block_id, value)(),
                 SchedulerCommitOutcome::SchedulerCommitOK);
        ret = {.id = proposed.block_id,
               .storage_long_hash = {},
               .value = value};
      }
      CHECK_EQ(num_nominated_txs, init_tx.size());
    }

    info("Initialized loadgen experiment with context_id {}", context_id_);
    return ret;
  }

  x23::Async<ParallelSignedTransactionBatch> get_transactions(size_t num_txs)
  {
    LoadgenExtensionResult result =
        co_await client_->extend_experiment(context_id_, num_txs,
                                            1'000'000'000);
    CHECK_EQ(result.error(), LoadgenRPCError::kLoadgenRPCErrorOK);
    co_return result.success().work_txs;
  }

  LoadgenContextID context_id_;

private:
  std::unique_ptr<loadgen_client_t> client_;
};

class LoadgenClientManager {
public:
  static constexpr size_t kNumSchedClients = 10;
  static_assert(MempoolCoordinatorRpc::kMaxBlockSize % kNumSchedClients == 0);

  LoadgenClientManager(const std::string &loadgen_address,
                       sched_coord_client_t &sched_coord_client,
                       CoordinatorSchedRpc &coord_sched_rpc,
                       size_t num_accounts)
    : loadgen_client_(loadgen_address), sched_coord_client_(sched_coord_client),
      coord_sched_rpc_(coord_sched_rpc)
  {
    info("Initializing loadgen experiment...");
    init_block_db_tip_ =
        loadgen_client_.init_experiment(sched_coord_client_, coord_sched_rpc_,
                                        num_accounts);
    info("Done initializing experiment. Now running.");
  }

  x23::Detached run(bool print_stats)
  {
    DbTip db_tip = init_block_db_tip_;

    // We do not count the first 20 blocks when measuring stats since the system
    // needs time to warm up.
    constexpr size_t kIgnoreFirstNBlocks = 20;
    std::chrono::steady_clock::time_point start;
    size_t total_transactions_nominated = 0;

    ParallelSignedTransactionBatch txs =
        co_await loadgen_client_.get_transactions(kLoadgenTxBatchSize);
    constexpr ValidatorID kValidatorId{};
    co_await sched_coord_client_.schedulercoord_submit_vetted(kValidatorId,
                                                              txs);

    size_t block_iteration_count = 0;
    while (!shutdown_flag_.load(std::memory_order_acquire)) {
      block_iteration_count++;
      const std::chrono::steady_clock::time_point block_start =
          std::chrono::steady_clock::now();

      x23::Async<SchedulerProposalResult> propose_handle =
          sched_coord_client_.schedulercoord_propose_block(db_tip.id,
                                                           ProposalContext{});

      txs = co_await loadgen_client_.get_transactions(kLoadgenTxBatchSize);
      x23::Async<void> submit_vetted_handle =
          sched_coord_client_.schedulercoord_submit_vetted(kValidatorId, txs);

      SchedulerProposalResult res_propose = co_await propose_handle;
      CHECK_EQ(res_propose.outcome(),
               SchedulerProposalOutcome::SchedulerProposalOK);
      const SchedulerProposedBlock &proposed = res_propose.proposed_block();
      ConsensusValue value{};
      value.prev_hash = sha256(db_tip.value);
      value.proposal_header = proposed.proposal.header;
      value.proposal_txs_hash = sha256(proposed.proposal.transactions);
      x23::Async<SchedulerCommitOutcome> commit_handle =
          sched_coord_client_.schedulercoord_commit_block(proposed.block_id,
                                                          value);

      co_await submit_vetted_handle;

      ElapsedTimer timer;
      CHECK_EQ(co_await commit_handle,
               SchedulerCommitOutcome::SchedulerCommitOK);
      info("commit handle took {} seconds", timer.elapsed());

      size_t num_txs = 0;
      {
        SignedTransactionBatch proposed_txs;
        CHECK(co_await parallel_batch_to_t(proposed.txs, proposed_txs));
        num_txs = proposed_txs.size();
      }

      db_tip = {.id = proposed.block_id,
                .storage_long_hash = {},
                .value = std::move(value)};

      if (print_stats && block_iteration_count > kIgnoreFirstNBlocks) {
        // Compute and print the throughputs.

        total_transactions_nominated += num_txs;
        const std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now();
        int64_t block_duration_us =
            std::chrono::duration_cast<std::chrono::microseconds>(now -
                                                                  block_start)
                .count();
        double block_duration_sec =
            block_duration_us / static_cast<double>(1'000'000);
        int64_t overall_duration_us =
            std::chrono::duration_cast<std::chrono::microseconds>(now - start)
                .count();
        double overall_duration_sec =
            overall_duration_us / static_cast<double>(1'000'000);
        CHECK_GT(block_duration_us, 0L);
        CHECK_GT(block_duration_sec, 0.0);
        CHECK_GT(overall_duration_us, 0L);
        CHECK_GT(overall_duration_sec, 0.0);

        double block_throughput_sec = num_txs / block_duration_sec;
        double overall_throughput_sec =
            total_transactions_nominated / overall_duration_sec;
        info("Block duration (s), {:.2f}, Num txs in block, {}, Overall "
             "duration (s), {:.2f}, Overall number of txs, {}",
             block_duration_sec, num_txs, overall_duration_sec,
             total_transactions_nominated);
        info("Block throughput (tx/s), {:.2f}, Overall throughput (tx/s), "
             "{:.2f}",
             block_throughput_sec, overall_throughput_sec);
      }
      else {
        start = std::chrono::steady_clock::now();
        total_transactions_nominated = 0;
      }
    }
  }

  ~LoadgenClientManager()
  {
    shutdown_flag_.store(true, std::memory_order_release);
  }

private:
  // The number of txs we request at a time from loadgen.
  static constexpr size_t kLoadgenTxBatchSize =
      MempoolCoordinatorRpc::kMaxBlockSize;
  // The number of txs we submit at a time to the mempool.
  static constexpr size_t kSchedTxBatchSize =
      MempoolCoordinatorRpc::kMaxBlockSize;

  LoadgenClient loadgen_client_;
  sched_coord_client_t &sched_coord_client_;
  CoordinatorSchedRpc &coord_sched_rpc_;
  DbTip init_block_db_tip_;

  std::atomic<bool> shutdown_flag_ = false;
};

} // namespace hogd
