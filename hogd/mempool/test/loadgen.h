#pragma once

#include "hogd/base/log.h"
#include "hogd/base/time.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/shared.h"

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
            /*max_message_size=*/LoadgenRpc::kRpcMaxMessageSize));
  }

  // TODO(jhumphri): Add flag to infra.h to disable existing setup for tests. We
  // do not want that setup when using loadgen.
  template<typename SchedCoordClient>
  DbTip init_experiment(SchedCoordClient &sched_coord_client,
                        uint32_t num_accounts)
  {
    // Initialize the experiment.
    ExperimentConfig config(ExperimentType::kExperimentTypeERC20Payments);
    config.erc20_payments().num_accounts = num_accounts;

    LoadgenResult result = client_->generate_experiment(config)();
    CHECK_EQ(result.error(), LoadgenRPCError::kLoadgenRPCErrorOK);
    context_id_ = result.success().context_id;

    // Pass the genesis block to the db.
    CHECK_EQ(sched_coord_client.schedcoord_load_genesis_block(
                 result.success().genesis_block)(),
             DbSchedResult::DbSchedResultOK);

    // We need to nominate and commit the initialization transactions. So first,
    // we start by adding them to the mempool with
    // [schedulercoord_submit_vetted()].
    const x23::Vec<InitializationBlock> &init_txs =
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
        nomination = res_nominate.scheduler_nomination();

        num_nominated_txs += nomination.nomination.transaction_hashes.size();
      }
      CHECK_EQ(num_nominated_txs, init_tx.size());

      ret = {.height = nomination.nomination.block_height,
             .hash = nomination.nomination.block_hash,
             .id = nomination.block_id};
    }

    info("Initialized loadgen experiment with context_id {}", context_id_);
    return ret;
  }

  SignedTransactionBatch get_transactions(size_t num_txs)
  {
    LoadgenExtensionResult result =
        client_->extend_experiment(context_id_, num_txs, 1'000'000'000)();
    CHECK_EQ(result.error(), LoadgenRPCError::kLoadgenRPCErrorOK);
    return result.success().work_txs;
  }

private:
  std::unique_ptr<loadgen_client_t> client_;
  LoadgenContextID context_id_;
};

class LoadgenClientManager {
public:
  LoadgenClientManager(const std::string &loadgen_address,
                       sched_coord_client_t &sched_coord_client,
                       size_t num_accounts)
    : loadgen_client_(loadgen_address), sched_coord_client_(sched_coord_client)
  {
    info("Initializing loadgen experiment...");
    init_block_db_tip_ =
        loadgen_client_.init_experiment(sched_coord_client_, num_accounts);
    info("Done initializing experiment. Now running.");
  }

  void run(bool print_stats)
  {
    DbTip db_tip = init_block_db_tip_;

    const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    size_t total_transactions_nominated = 0;
    while (!shutdown_flag_.load(std::memory_order_acquire)) {
      const std::chrono::steady_clock::time_point block_start =
          std::chrono::steady_clock::now();

      auto timer = std::make_unique<ElapsedTimer>();
      const SignedTransactionBatch txs =
          loadgen_client_.get_transactions(kLoadgenTxBatchSize);
      double get_tx_duration = timer->elapsed();

      timer = std::make_unique<ElapsedTimer>();
      ValidatorID validator_id{};
      sched_coord_client_.schedulercoord_submit_vetted(validator_id, txs)();
      double submit_vetted_duration = timer->elapsed();

      timer = std::make_unique<ElapsedTimer>();
      auto res_nominate = sched_coord_client_.schedulercoord_nominate(db_tip)();
      CHECK_EQ(res_nominate.outcome(),
               SchedulerNominationOutcome::SchedulerNominationOK);
      double nominate_duration = timer->elapsed();
      info(
          "get_tx_duration {}, submit_vetted_duration {}, nominate_duration {}",
          get_tx_duration, submit_vetted_duration, nominate_duration);

      const SchedulerNomination nomination =
          res_nominate.scheduler_nomination();
      db_tip = {.height = nomination.nomination.block_height,
                .hash = nomination.nomination.block_hash,
                .id = nomination.block_id};
      total_transactions_nominated +=
          nomination.nomination.transaction_hashes.size();

      if (print_stats) {
        // Compute and print the throughputs.
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

        double block_throughput_sec =
            nomination.nomination.transaction_hashes.size() /
            block_duration_sec;
        double overall_throughput_sec =
            total_transactions_nominated / overall_duration_sec;
        info("Block duration (s), {:.2f}, Num txs in block, {}, Overall "
             "duration (s), {:.2f}, Overall number of txs, {}",
             block_duration_sec,
             nomination.nomination.transaction_hashes.size(),
             overall_duration_sec, total_transactions_nominated);
        info("Block throughput (tx/s), {:.2f}, Overall throughput (tx/s), "
             "{:.2f}",
             block_throughput_sec, overall_throughput_sec);
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

  LoadgenClient loadgen_client_;
  sched_coord_client_t &sched_coord_client_;
  DbTip init_block_db_tip_;

  std::atomic<bool> shutdown_flag_ = false;
};

} // namespace hogd
