#pragma once

#include "hogd/base/log.h"
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
    client_ = std::make_unique<loadgen_client_t>(std::move(
        create_client<LoadgenInterfaceV1>(host_port, /*retry=*/true)));
  }

  // TODO(jhumphri): Add flag to infra.h to disable existing setup for tests. We
  // do not want that setup when using loadgen.
  DbTip init_experiment(MempoolCoordinatorRpc &mempool_coord_rpc,
                        uint32_t num_accounts)
  {
    // Initialize the experiment.
    ExperimentConfig config(ExperimentType::kExperimentTypeERC20Payments);
    config.erc20_payments().num_accounts = num_accounts;

    LoadgenResult result = client_->generate_experiment(config)();
    CHECK_EQ(result.error(), LoadgenRPCError::kLoadgenRPCErrorOK);
    context_id_ = result.success().context_id;

    // Pass the genesis block to the db.
    CHECK_EQ(mempool_coord_rpc.send_genesis_block(
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
      mempool_coord_rpc.schedulercoord_submit_vetted(kValidatorId, init_tx)();

      // Now nominate and commit the initialization transactions with
      // [schedulercoord_nominate()].
      size_t num_nominated_txs = 0;
      SchedulerNomination nomination{};
      while (num_nominated_txs < init_tx.size()) {
        DbTip db_tip{.height = nomination.nomination.block_height,
                     .hash = nomination.nomination.block_hash,
                     .id = nomination.block_id};
        auto res_nominate = mempool_coord_rpc.schedulercoord_nominate(db_tip)();
        mempool_coord_rpc.dump_stats();
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
                       MempoolCoordinatorRpc &mempool, size_t num_accounts)
    : loadgen_client_(loadgen_address), mempool_(mempool),
      worker_([this] { work(); })
  {
    init_block_db_tip_ = loadgen_client_.init_experiment(mempool, num_accounts);
  }

  ~LoadgenClientManager()
  {
    shutdown_flag_.store(true, std::memory_order_release);
  }

private:
  // The number of txs we request at a time from loadgen.
  static constexpr size_t kLoadgenTxBatchSize = 10'000;

  void work()
  {
    DbTip db_tip = init_block_db_tip_;

    while (!shutdown_flag_.load(std::memory_order_acquire)) {
      const SignedTransactionBatch txs =
          loadgen_client_.get_transactions(kLoadgenTxBatchSize);

      ValidatorID validator_id{};
      mempool_.schedulercoord_submit_vetted(validator_id, txs)();

      auto res_nominate = mempool_.schedulercoord_nominate(db_tip)();
      CHECK_EQ(res_nominate.outcome(),
               SchedulerNominationOutcome::SchedulerNominationOK);

      const SchedulerNomination nomination =
          res_nominate.scheduler_nomination();
      db_tip = {.height = nomination.nomination.block_height,
                .hash = nomination.nomination.block_hash,
                .id = nomination.block_id};
    }
  }

  LoadgenClient loadgen_client_;
  MempoolCoordinatorRpc &mempool_;
  DbTip init_block_db_tip_;

  std::atomic<bool> shutdown_flag_ = false;
  std::jthread worker_;
};

} // namespace hogd
