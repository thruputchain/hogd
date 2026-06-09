#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>

#include "exec_result.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/init.h"
#include "hogd/loadgen/erc20_experiment.h"
#include "loadgen.hh"
#include "transaction.hh"
#include "types.hh"

// In-process benchmark of the contract runtimes on the erc20 + simple_wallet
// payments workload. Unlike payments_bench (which drives a distributed
// bench_server / coordinator over RPC), this runs everything in one process
// against an in-memory hogdb via NativeContractTestFramework -- nothing to
// stand up, just run the binary locally.
//
// It times the *execution* of work transactions (root_invoke only, no commit,
// matching fibonacci_*only's uncommitted path) so the number reflects
// interpreter + syscall cost rather than DB write-back. It reports both the
// wasm and rv32 builds so the two runtimes can be compared head to head, and
// prints gas/tx for each (the rv32 interpreter meters one gas per instruction,
// so its gas figure is naturally higher than wasm's coarser fuel metering --
// that is an accounting-scale difference, not extra work).

namespace hogd {
namespace {

struct BenchResult {
  uint64_t num_txs = 0;
  uint64_t best_us = 0; // best (fastest) rep, microseconds
  uint64_t gas_per_tx = 0;
};

// Deploys the erc20 + wallet contracts (wasm or rv32) and mints balances, then
// times executing num_txs wallet-authorized transfers, repeated for nreps.
BenchResult
run_payments(bool use_rv32, uint32_t num_accounts, uint64_t num_txs,
             int nreps)
{
  NativeContractTestFramework framework(
      /*log=*/false, BlockExecutionMode::kBlockExecutionModeProposal,
      TestDB::kHogDB, /*empty_genesis=*/false);

  auto context = make_hogexec_context();

  ERC20ExperimentState exp(
      ERC20PaymentsConfig{
          .num_addresses = num_accounts,
      },
      use_rv32);

  auto init_result = exp.generate_experiment()();
  if (!init_result.has_value()) {
    throw std::runtime_error("failed to generate erc20 experiment");
  }

  framework.run_genesis_initialization(
      init_result.value().genesis_configuration);
  for (auto const &init_block : init_result.value().initialization_txs) {
    for (auto const &tx : init_block) {
      auto handler = framework.make_new_system_call_handler(tx.tx, *context);
      if (!handler.root_invoke().is_root_success()) {
        throw std::runtime_error("initialization transaction failed");
      }
      if (handler.try_commit_transaction() != ExecResult::ExecResultOK) {
        throw std::runtime_error("failed to commit initialization transaction");
      }
      // mint/initialize emit events; drain them so the committed handler isn't
      // destroyed with events still pending.
      if (!handler.emit_events(ExecResult::ExecResultOK)().has_value()) {
        throw std::runtime_error("failed to emit initialization events");
      }
    }
    framework.advance_block();
  }

  const BlockHeight expiry_height = 1'000'000;
  auto extend_result = exp.extend_experiment(num_txs, expiry_height)();
  if (!extend_result.has_value()) {
    throw std::runtime_error("failed to extend erc20 experiment");
  }
  SignedTransactionBatch work_txs;
  parallel_batch_to_t(extend_result.value().work_txs, work_txs)();
  if (work_txs.size() != num_txs) {
    throw std::runtime_error("unexpected work tx count");
  }

  // Execution-only timing: root_invoke runs the full contract (wallet
  // do_action -> ed25519 verify -> cross-call erc20 transfer), then we abort
  // rather than commit. abort_transaction() rolls back the staged writes and
  // discards the transfer's events, so every rep executes against the same
  // post-init state, the replay cache is untouched, and there is no DB
  // write-back in the timed region -- the number reflects interpreter + syscall
  // cost. gas_consumed is deterministic across reps, so we capture it once.
  BenchResult result;
  result.num_txs = num_txs;
  uint64_t gas_per_tx = 0;

  // Warm up (lazy translation, instruction cache) before the timed reps.
  for (auto const &tx : work_txs) {
    auto handler = framework.make_new_system_call_handler(tx.tx, *context);
    if (!handler.root_invoke().is_root_success()) {
      throw std::runtime_error("work transaction failed during warmup");
    }
    [[maybe_unused]] auto aborted = handler.abort_transaction();
  }

  for (int rep = 0; rep < nreps; ++rep) {
    uint64_t total_gas = 0;
    auto start = std::chrono::steady_clock::now();
    for (auto const &tx : work_txs) {
      auto handler = framework.make_new_system_call_handler(tx.tx, *context);
      auto invoke_result = handler.root_invoke();
      if (!invoke_result.is_root_success()) {
        throw std::runtime_error("work transaction failed");
      }
      total_gas += invoke_result.gas_consumed_;
      [[maybe_unused]] auto aborted = handler.abort_transaction();
    }
    auto finish = std::chrono::steady_clock::now();
    const uint64_t us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(finish - start)
            .count());
    if (result.best_us == 0 || us < result.best_us) {
      result.best_us = us;
    }
    gas_per_tx = total_gas / num_txs;
  }
  result.gas_per_tx = gas_per_tx;
  return result;
}

void
print_result(std::string_view label, BenchResult const &r)
{
  const double tps = static_cast<double>(r.num_txs) * 1'000'000.0 /
                     static_cast<double>(r.best_us);
  const double us_per_tx =
      static_cast<double>(r.best_us) / static_cast<double>(r.num_txs);
  std::println("{:>5} | {:>8} txs | {:>9} us | {:>12.0f} tx/s | {:>8.2f} us/tx "
               "| {:>10} gas/tx",
               label, r.num_txs, r.best_us, tps, us_per_tx, r.gas_per_tx);
}

void
run(uint32_t num_accounts, uint64_t num_txs, int nreps, bool do_wasm,
    bool do_rv32)
{
  std::println("payments exec bench: {} accounts, {} txs, {} reps (best rep "
               "reported)\n",
               num_accounts, num_txs, nreps);

  std::optional<BenchResult> wasm_res;
  std::optional<BenchResult> rv32_res;
  if (do_wasm) {
    wasm_res = run_payments(/*use_rv32=*/false, num_accounts, num_txs, nreps);
    print_result("wasm", *wasm_res);
  }
  if (do_rv32) {
    rv32_res = run_payments(/*use_rv32=*/true, num_accounts, num_txs, nreps);
    print_result("rv32", *rv32_res);
  }

  if (wasm_res && rv32_res) {
    const double speed_ratio = static_cast<double>(rv32_res->best_us) /
                               static_cast<double>(wasm_res->best_us);
    const double gas_ratio = static_cast<double>(rv32_res->gas_per_tx) /
                             static_cast<double>(wasm_res->gas_per_tx);
    std::println("\nrv32 vs wasm: {:.2f}x execution time, {:.2f}x gas/tx",
                 speed_ratio, gas_ratio);
  }
}

} // namespace
} // namespace hogd

namespace {
void
usage(char *argv[])
{
  std::println("Usage: {} [-a <num_accounts>] [-n <num_txs>] [-k <reps>] "
               "[-r | -w]",
               argv[0]);
  std::println("  -a  number of accounts (default 10)");
  std::println("  -n  work transactions per rep (default 5000)");
  std::println("  -k  timed reps, fastest reported (default 5)");
  std::println("  -r  rv32 contracts only (default: run both)");
  std::println("  -w  wasm contracts only (default: run both)");
  exit(EXIT_FAILURE);
}
} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret) {
    return ret;
  }

  uint32_t num_accounts = 10;
  uint64_t num_txs = 5000;
  int nreps = 5;
  bool only_rv32 = false;
  bool only_wasm = false;

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "a:n:k:rwh")) != -1) {
    switch (opt) {
    case 'a':
      num_accounts = std::stoul(optarg);
      break;
    case 'n':
      num_txs = std::stoull(optarg);
      break;
    case 'k':
      nreps = std::stoi(optarg);
      break;
    case 'r':
      only_rv32 = true;
      break;
    case 'w':
      only_wasm = true;
      break;
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)
  if (only_rv32 && only_wasm) {
    usage(argv);
  }

  try {
    hogd::run(num_accounts, num_txs, nreps, /*do_wasm=*/!only_rv32,
              /*do_rv32=*/!only_wasm);
  } catch (std::exception const &e) {
    hogd::error("{}", e.what());
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
