#include <cstdint>
#include <cstdlib>
#include <exception>
#include <print>
#include <stdexcept>
#include <string>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/init.h"
#include "hogd/loadgen/erc20_experiment.h"
#include "loadgen.hh"
#include "rpc_types.hh"
#include "types.hh"

namespace hogd {
namespace {

void
run_gas_usage_metric(uint32_t num_accounts)
{
  // Set up test framework (spins up DB server, executor, etc. in-process)
  NativeContractTestFramework framework(
      /*log=*/false, BlockExecutionMode::kBlockExecutionModeProposal,
      TestDB::kHogDB, false);

  // Create ERC20 experiment
  auto exp = ERC20ExperimentState(ERC20PaymentsConfig{
      .num_accounts = num_accounts,
  });

  // Generate experiment (creates genesis configuration and init txs)
  auto init_result = exp.generate_experiment();
  if (!init_result.has_value()) {
    throw std::runtime_error("Failed to generate ERC20 experiment");
  }

  // Run genesis configuration initialization
  framework.run_genesis_initialization(
      init_result.value().genesis_configuration);

  // Run initialization transactions
  for (auto const &init_block : init_result.value().initialization_txs) {
    for (auto const &tx : init_block) {
      // Create a new runtime context for each transaction (it's moved into
      // handler)
      auto handler =
          framework.make_new_system_call_handler(tx, make_hogexec_context());
      auto invoke_result = handler.root_invoke();
      if (!invoke_result.is_root_success()) {
        throw std::runtime_error("Initialization transaction execution failed");
      }
      if (auto r = handler.try_commit_transaction();
          !r.has_value() || !r.value()) {
        throw std::runtime_error("Failed to commit initialization transaction");
      }
    }
    framework.advance_block();
  }

  // Extend experiment to get one work transaction
  const BlockHeight expiry_height = 1'000'000;
  auto extend_result = exp.extend_experiment(1, expiry_height);
  if (!extend_result.has_value()) {
    throw std::runtime_error("Failed to extend ERC20 experiment");
  }

  if (extend_result.value().work_txs.empty()) {
    throw std::runtime_error("No transactions generated from extend");
  }

  // Execute the first transaction from extend()
  auto const &tx = extend_result.value().work_txs[0];
  std::println("Executing transaction...");

  // Create a new runtime context for the transaction (it's moved into handler)
  auto handler =
      framework.make_new_system_call_handler(tx, make_hogexec_context());
  auto invoke_result = handler.root_invoke();

  if (!invoke_result.is_root_success()) {
    throw std::runtime_error("Transaction execution failed");
  }

  // Commit the transaction
  if (auto r = handler.try_commit_transaction(); !r.has_value() || !r.value()) {
    throw std::runtime_error("Failed to commit transaction");
  }

  // Print gas consumption
  std::println("ERC20 Payment Experiment: Gas consumed: {}",
               invoke_result.gas_consumed_);
}

} // namespace
} // namespace hogd

namespace {
void
usage(char *argv[])
{
  std::println("Usage: {} [-a <num_accounts>]", argv[0]);
  std::println("  -a <num_accounts>  Number of accounts (default: 2)");
  exit(EXIT_FAILURE);
}
} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;

  uint32_t num_accounts = 2; // default

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "a:h")) != -1) {
    switch (opt) {
    case 'a':
      num_accounts = std::stoi(optarg);
      break;
    case 'h':
      usage(argv);
      break;
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)

  try {
    hogd::run_gas_usage_metric(num_accounts);
  } catch (std::exception const &e) {
    hogd::error("{}", e.what());
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
