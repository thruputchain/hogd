#include <benchmark/benchmark.h>

#include <memory>
#include <stdexcept>

#include "exec_result.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/init.h"
#include "hogd/loadgen/erc20_experiment.h"
#include "loadgen.hh"
#include "transaction.hh"

// In-process benchmark of the contract runtimes on the erc20 + simple_wallet
// payments workload. Unlike payments_bench (which drives a distributed
// bench_server / coordinator over RPC), this runs entirely in one process
// against an in-memory hog_db via NativeContractTestFramework -- nothing to
// stand up, just run the binary locally.
//
// Timing/averaging/stats are handled by google benchmark. The timed body
// executes one work transaction via root_invoke + abort: abort rolls back the
// staged writes and discards the transfer's events, so each iteration runs
// against the same post-init state with no DB write-back and the replay cache
// untouched -- the measured time reflects interpreter + syscall cost. gas/tx
// is reported as a counter.

namespace hogd {
namespace {

class PaymentsExecBench : public benchmark::Fixture {
public:
  void SetUp(benchmark::State &state) override
  {
    const auto num_accounts = static_cast<uint32_t>(state.range(0));
    // A pool of distinct work txs to cycle through; abort keeps them reusable.
    constexpr uint64_t kPoolTxs = 1000;

    try {
      framework_ = std::make_unique<NativeContractTestFramework>(
          /*log=*/false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb, /*empty_genesis=*/false);
      context_ = make_hog_exec_context();

      ERC20ExperimentState exp(ERC20PaymentsConfig{
          .num_addresses = num_accounts,
      });

      auto init_result = exp.generate_experiment()();
      if (!init_result.has_value()) {
        throw std::runtime_error("failed to generate erc20 experiment");
      }
      framework_->run_genesis_initialization(
          init_result.value().genesis_configuration);
      for (auto const &init_block : init_result.value().initialization_txs) {
        for (auto const &tx : init_block) {
          auto handler =
              framework_->make_new_system_call_handler(tx.tx, *context_);
          if (!handler.root_invoke().is_root_success()) {
            throw std::runtime_error("initialization transaction failed");
          }
          if (handler.try_commit_transaction() != ExecResult::ExecResultOK) {
            throw std::runtime_error("failed to commit init transaction");
          }
          // mint/initialize emit events; drain them so the committed handler
          // isn't destroyed with events still pending.
          if (!handler
                   .emit_events(ExecResult::ExecResultOK,
                                ProposedTransaction{.signed_tx = tx})()
                   .has_value()) {
            throw std::runtime_error("failed to emit init events");
          }
        }
        framework_->advance_block();
      }

      auto extend_result = exp.extend_experiment(kPoolTxs, 1'000'000)();
      if (!extend_result.has_value()) {
        throw std::runtime_error("failed to extend erc20 experiment");
      }
      parallel_batch_to_t(extend_result.value().work_txs, work_txs_)();
      if (work_txs_.empty()) {
        throw std::runtime_error("no work transactions generated");
      }
    } catch (std::exception const &e) {
      state.SkipWithError(e.what());
    }
  }

  void TearDown(benchmark::State & /*state*/) override
  {
    work_txs_ = SignedTransactionBatch{};
    context_.reset();
    framework_.reset();
  }

protected:
  std::unique_ptr<NativeContractTestFramework> framework_;
  std::unique_ptr<RuntimeContext> context_;
  SignedTransactionBatch work_txs_;
};

BENCHMARK_DEFINE_F(PaymentsExecBench, transfer)(benchmark::State &state)
{
  if (work_txs_.empty()) {
    return; // SetUp already called SkipWithError.
  }

  uint64_t gas_per_tx = 0;
  size_t i = 0;
  for (auto _ : state) {
    auto const &tx = work_txs_[i++ % work_txs_.size()];
    auto handler = framework_->make_new_system_call_handler(tx.tx, *context_);
    auto invoke_result = handler.root_invoke();
    if (!invoke_result.is_root_success()) {
      state.SkipWithError("work transaction failed");
      break;
    }
    gas_per_tx = invoke_result.gas_consumed_;
    [[maybe_unused]] auto aborted = handler.abort_transaction();
  }

  state.SetItemsProcessed(state.iterations());
  state.counters["gas/tx"] = static_cast<double>(gas_per_tx);
  state.counters["tx/s"] =
      benchmark::Counter(static_cast<double>(state.iterations()),
                         benchmark::Counter::kIsRate);
}

// num_accounts: a couple of account counts so contention effects are visible.
BENCHMARK_REGISTER_F(PaymentsExecBench, transfer)
    ->Args({10})
    ->Args({2})
    ->ArgNames({"accounts"})
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();

} // namespace
} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret) {
    return ret;
  }
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
