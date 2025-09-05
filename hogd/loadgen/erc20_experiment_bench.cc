#include <benchmark/benchmark.h>

#include <cassert>
#include <cstdlib>
#include <stdexcept>

#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/loadgen/erc20_experiment.h"
#include "loadgen.hh"

namespace hogd {

namespace {

// Test different numbers of accounts to see scaling behavior
constexpr uint32_t kMinAccounts = 10;
constexpr uint32_t kMaxAccounts = 1000;

// Test different transaction batch sizes to measure parallelization benefits
constexpr uint64_t kMinTxs = 1000;
constexpr uint64_t kMaxTxs = 100000;

void
bm_erc20_generate_experiment(benchmark::State &state)
{
  const auto num_accounts = static_cast<uint32_t>(state.range(0));

  for (auto _ : state) {
    auto exp = ERC20ExperimentState(ERC20PaymentsConfig{
        .num_accounts = num_accounts,
    });

    auto result = exp.generate_experiment();
    if (!result.has_value()) {
      state.SkipWithError("Failed to generate ERC20 experiment");
      return;
    }

    // Prevent compiler optimizations
    benchmark::DoNotOptimize(result.value());
  }

  // Report accounts processed per iteration
  state.counters["accounts"] =
      benchmark::Counter(static_cast<double>(num_accounts),
                         benchmark::Counter::kIsIterationInvariantRate);
}

void
bm_erc20_extend_experiment(benchmark::State &state)
{
  const uint32_t num_accounts = 100; // Fixed number of accounts
  const auto num_txs = static_cast<uint64_t>(state.range(0));

  // Setup the experiment once
  auto exp = ERC20ExperimentState(ERC20PaymentsConfig{
      .num_accounts = num_accounts,
  });

  auto init_result = exp.generate_experiment();
  if (!init_result.has_value()) {
    state.SkipWithError("Failed to initialize ERC20 experiment");
    return;
  }

  for (auto _ : state) {
    auto result = exp.extend_experiment(num_txs);
    if (!result.has_value()) {
      throw std::runtime_error("Failed to extend ERC20 experiment");
    }

    // Verify we got the expected number of transactions
    assert(result.value().work_txs.size() == num_txs);

    // Prevent compiler optimizations
    benchmark::DoNotOptimize(result.value());
  }

  // Report throughput metrics
  state.counters["txs_per_sec"] =
      benchmark::Counter(static_cast<double>(num_txs),
                         benchmark::Counter::kIsIterationInvariantRate);
}

} // namespace

} // namespace hogd

// Register benchmarks

namespace {
// Test experiment generation scaling with number of accounts
BENCHMARK(hogd::bm_erc20_generate_experiment)
    ->RangeMultiplier(2)
    ->Range(hogd::kMinAccounts, hogd::kMaxAccounts)
    ->Unit(benchmark::kMillisecond); // NOLINT

// Test transaction generation scaling with batch size
BENCHMARK(hogd::bm_erc20_extend_experiment)
    ->RangeMultiplier(2)
    ->Range(hogd::kMinTxs, hogd::kMaxTxs)
    ->Unit(benchmark::kMillisecond); // NOLINT
} // namespace

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret)
    return EXIT_FAILURE;

  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  return 0;
}
