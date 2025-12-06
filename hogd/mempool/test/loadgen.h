#pragma once

#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "coord_sched.hh"
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
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
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
      throw std::runtime_error(
          std::format("Invalid loadgen address: {}", loadgen_address));
    }

    // Connect to the loadgen server.
    client_ =
        std::make_unique<loadgen_client_t>(create_client<LoadgenInterfaceV1>(
            host_port, /*retry=*/true,
            /*max_message_size=*/LoadgenRpc::kRpcMaxMessageSize)());
  }

  // TODO(jhumphri): Add flag to infra.h to disable existing setup for tests. We
  // do not want that setup when using loadgen.
  template<typename SchedCoordClient>
  DbTip init_experiment(SchedCoordClient &sched_coord_client,
                        CoordinatorSchedRpc &coord_sched_rpc,
                        uint32_t num_accounts)
  {
    // Initialize the experiment.
    ExperimentConfig config(ExperimentType::kExperimentTypeERC20Payments);
    config.erc20_payments().num_accounts = num_accounts;

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
    DbTip ret{};
    for (const InitializationBlock &init_tx : init_txs) {
      sched_coord_client.schedulercoord_submit_vetted(kValidatorId, init_tx)();

      // Now nominate and commit the initialization transactions with
      // [schedulercoord_nominate()].
      size_t num_nominated_txs = 0;
      SchedulerNomination nomination{};
      while (num_nominated_txs < init_tx.size()) {
        DbTip db_tip{.height = nomination.nomination.block_height,
                     .hash = nomination.nomination.block_hash,
                     .id = nomination.block_id};
        auto res_nominate =
            sched_coord_client.schedulercoord_nominate(db_tip)();
        CHECK_EQ(res_nominate.outcome(),
                 SchedulerNominationOutcome::SchedulerNominationOK);
        nomination = coord_sched_rpc.get_nomination()();
        CHECK_EQ(res_nominate.block_id(), nomination.block_id);
        num_nominated_txs += nomination.nomination.transaction_hashes.size();

        CHECK_EQ(sched_coord_client.schedulercoord_commit_block(
                     res_nominate.block_id())(),
                 SchedulerCommitOutcome::SchedulerCommitOK);
      }
      CHECK_EQ(num_nominated_txs, init_tx.size());

      ret = {.height = nomination.nomination.block_height,
             .hash = nomination.nomination.block_hash,
             .id = nomination.block_id};
    }

    info("Initialized loadgen experiment with context_id {}", context_id_);
    return ret;
  }

  x23::Async<SignedTransactionBatch> get_transactions(size_t num_txs)
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
                       std::vector<sched_coord_client_t> &sched_coord_clients,
                       CoordinatorSchedRpc &coord_sched_rpc,
                       size_t num_accounts)
    : sched_coord_clients_(sched_coord_clients),
      coord_sched_rpc_(coord_sched_rpc)
  {
    CHECK_EQ(sched_coord_clients_.size(), kNumSchedClients);

    for (size_t i = 0; i < kNumLoadgenClients; i++) {
      loadgen_clients_.emplace_back(loadgen_address);
    }

    info("Initializing loadgen experiment...");
    init_block_db_tip_ =
        loadgen_clients_[0].init_experiment(sched_coord_clients_[0],
                                            coord_sched_rpc_, num_accounts);
    for (size_t i = 1; i < kNumLoadgenClients; i++) {
      loadgen_clients_[i].context_id_ = loadgen_clients_[0].context_id_;
    }
    info("Done initializing experiment. Now running.");
  }

  x23::Detached get_transactions(x23::Vec<SignedTransaction> &txs,
                                 x23::CoroCB<void> cb)
  {
    co_await default_coropoll().pool_.bg();

    x23::Vec<x23::Async<SignedTransactionBatch>> handles;
    for (size_t i = 0; i < kNumLoadgenClients; i++) {
      handles.push_back(
          loadgen_clients_[i].get_transactions(kLoadgenTxBatchSize));
    }
    for (size_t i = 0; i < kNumLoadgenClients; i++) {
      txs += co_await handles[i];
    }
    cb();
  }

  x23::Detached submit_vetted(x23::Vec<SignedTransaction> &txs,
                              x23::CoroCB<void> cb)
  {
    co_await default_coropoll().pool_.bg();

    constexpr ValidatorID kValidatorId{};
    x23::Vec<x23::Async<void>> handles;
    for (size_t i = 0; i < kNumSchedClients; i++) {
      x23::span tx_chunk(txs.begin() + (i * kSchedTxBatchSize),
                         txs.begin() + ((i + 1) * kSchedTxBatchSize));
      handles.push_back(
          sched_coord_clients_[i].schedulercoord_submit_vetted(kValidatorId,
                                                               tx_chunk));
    }
    for (size_t i = 0; i < kNumLoadgenClients; i++) {
      co_await handles[i];
    }
    cb();
  }

  x23::Detached run(bool print_stats)
  {
    DbTip db_tip = init_block_db_tip_;

    // We do not count the first 20 blocks when measuring stats since the system
    // needs time to warm up.
    constexpr size_t kIgnoreFirstNBlocks = 20;
    std::chrono::steady_clock::time_point start;
    size_t total_transactions_nominated = 0;

    x23::Vec<SignedTransaction> txs;
    {
      x23::Awaitable<void> awaitable;
      get_transactions(txs, x23::CoroCB<void>(awaitable));
      co_await awaitable;
    }

    {
      x23::Awaitable<void> awaitable;
      submit_vetted(txs, x23::CoroCB<void>(awaitable));
      co_await awaitable;
    }

    size_t block_iteration_count = 0;
    while (!shutdown_flag_.load(std::memory_order_acquire)) {
      block_iteration_count++;
      const std::chrono::steady_clock::time_point block_start =
          std::chrono::steady_clock::now();

      x23::Async<SchedulerNominationResult> nominate_handle =
          sched_coord_clients_[0].schedulercoord_nominate(db_tip);

      txs.clear();
      x23::Awaitable<void> get_transactions_handle;
      get_transactions(txs, x23::CoroCB<void>(get_transactions_handle));
      co_await get_transactions_handle;

      x23::Awaitable<void> submit_vetted_handle;
      submit_vetted(txs, x23::CoroCB<void>(submit_vetted_handle));

      SchedulerNominationResult res_nominate = co_await nominate_handle;
      CHECK_EQ(res_nominate.outcome(),
               SchedulerNominationOutcome::SchedulerNominationOK);
      x23::Async<SchedulerCommitOutcome> commit_handle =
          sched_coord_clients_[0].schedulercoord_commit_block(
              res_nominate.block_id());

      co_await submit_vetted_handle;

      ElapsedTimer timer;
      CHECK_EQ(co_await commit_handle,
               SchedulerCommitOutcome::SchedulerCommitOK);
      info("commit handle took {} seconds", timer.elapsed());

      const auto &[nomination, num_txs] =
          coord_sched_rpc_.get_nomination_no_txs()();
      CHECK_EQ(res_nominate.block_id(), nomination.block_id);

      db_tip = {.height = nomination.nomination.block_height,
                .hash = nomination.nomination.block_hash,
                .id = nomination.block_id};

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
  static constexpr size_t kNumLoadgenClients = 10;
  static_assert(MempoolCoordinatorRpc::kMaxBlockSize % kNumLoadgenClients == 0);
  // The number of txs we request at a time from loadgen.
  static constexpr size_t kLoadgenTxBatchSize =
      MempoolCoordinatorRpc::kMaxBlockSize / kNumLoadgenClients;
  // The number of txs we submit at a time to the mempool.
  static constexpr size_t kSchedTxBatchSize =
      MempoolCoordinatorRpc::kMaxBlockSize / kNumSchedClients;

  std::vector<LoadgenClient> loadgen_clients_;
  std::vector<sched_coord_client_t> &sched_coord_clients_;
  CoordinatorSchedRpc &coord_sched_rpc_;
  DbTip init_block_db_tip_;

  std::atomic<bool> shutdown_flag_ = false;
};

} // namespace hogd
