#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <set>

#include "exec_result.hh"
#include "hogd/base/xdr_util.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/test_main.h"
#include "hogd/loadgen/erc20_basic_auth_experiment.h"
#include "hogd/loadgen/erc20_experiment.h"
#include "hogd/loadgen/status.h"
#include "loadgen.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// NOLINTNEXTLINE(misc-use-internal-linkage)
class ERC20ExperimentTest : public ::testing::Test,
                            public NativeContractTestFramework {
public:
  ERC20ExperimentTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb, false)
  {}

protected:
  std::set<Uniquifier> uniquifiers_;

  void check_uniquifier(const Uniquifier &uniquifier)
  {
    EXPECT_TRUE(uniquifiers_.insert(uniquifier).second);
  }

  void check_initial_balance()
  {
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    auto bal =
        lookup_storage_item_from_db(make_free_native_token_balance_addrkey(
            GenesisConfigurationLoader::kDeployContractAddress));
    ASSERT_TRUE(bal.has_value());
    EXPECT_LT(from_xdr(bal.value().nnint()),
              GenesisConfigurationLoader::initial_hog_token_supply());
    // TODO(geoff): assert balance = initial_supply - storage_cost(nnint)
    // NOLINTEND(bugprone-unchecked-optional-access)
  }

  std::unique_ptr<RuntimeContext> runtime_context_;

  void SetUp() override { runtime_context_ = make_hog_exec_context(); }

  // Runs the full erc20 payments experiment in-process: genesis deploy, the
  // initialization/mint blocks, then 100 wallet-authorized transfer work txs.
  void run_payments_experiment()
  {
    auto state = std::make_shared<ERC20ExperimentState>(
        ERC20PaymentsConfig{
            .num_addresses = 10,
        });
    auto result = state->generate_experiment()();
    ASSERT_TRUE(result.has_value());

    run_genesis_initialization(result.value().genesis_configuration);
    check_initial_balance();

    for (auto &init_block : result.value().initialization_txs) {
      for (auto &tx : init_block) {
        check_uniquifier(tx.tx.uniquifier);
        auto handler = make_new_system_call_handler(tx, *runtime_context_);
        auto invoke_res = handler.root_invoke();
        ASSERT_TRUE(invoke_res.is_root_success());
        auto res = handler.try_commit_transaction();
        ASSERT_TRUE(res == ExecResult::ExecResultOK);
        ASSERT_TRUE(
            handler.emit_events(ExecResult::ExecResultOK,
                                ProposedTransaction{.signed_tx = tx})());
      }

      advance_block();
    }

    std::optional<GasAmount> gas_used;

    LoadgenStatus<LoadgenExtension> work_txs_raw =
        state->extend_experiment(100, 10)();
    ASSERT_TRUE(work_txs_raw.has_value());

    SignedTransactionBatch work_txs;
    parallel_batch_to_t(work_txs_raw.value().work_txs, work_txs)();
    ASSERT_EQ(work_txs.size(), 100);
    for (auto &tx : work_txs) {
      check_uniquifier(tx.tx.uniquifier);
      auto handler = make_new_system_call_handler(tx, *runtime_context_);
      auto invoke_res = handler.root_invoke();
      ASSERT_TRUE(invoke_res.is_root_success());
      auto res = handler.try_commit_transaction();
      ASSERT_TRUE(res == ExecResult::ExecResultOK);
      if (!gas_used) {
        gas_used = invoke_res.gas_consumed_;
      }
      ASSERT_EQ(invoke_res.gas_consumed_, gas_used.value());
      ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK,
                                      ProposedTransaction{.signed_tx = tx})());
    }
  }
};

// Same payments workload, but the erc20 and wallet contracts are the compiled
// rv32 artifacts run through the RV32 runtime. Exercises the rv32 wallet
// do_action -> ed25519 verify -> cross-call erc20 transfer path end to end.
TEST_F(ERC20ExperimentTest, BasicRv32)
{
  run_payments_experiment();
}

TEST_F(ERC20ExperimentTest, BasicAuth)
{
  auto state =
      std::make_shared<ERC20BasicAuthExperimentState>(ERC20PaymentsConfig{
          .num_addresses = 10,
      });
  auto result = state->generate_experiment()();
  ASSERT_TRUE(result.has_value());

  run_genesis_initialization(result.value().genesis_configuration);
  check_initial_balance();

  for (auto &init_block : result.value().initialization_txs) {
    for (auto &tx : init_block) {
      check_uniquifier(tx.tx.uniquifier);
      auto handler = make_new_system_call_handler(tx, *runtime_context_);
      auto invoke_res = handler.root_invoke();
      ASSERT_TRUE(invoke_res.is_root_success());
      auto res = handler.try_commit_transaction();
      ASSERT_TRUE(res == ExecResult::ExecResultOK);
      ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK,
                                      ProposedTransaction{.signed_tx = tx})());
    }

    advance_block();
  }

  std::optional<GasAmount> gas_used;

  LoadgenStatus<LoadgenExtension> work_txs_raw =
      state->extend_experiment(100, 10)();
  ASSERT_TRUE(work_txs_raw.has_value());

  SignedTransactionBatch work_txs;
  parallel_batch_to_t(work_txs_raw.value().work_txs, work_txs)();
  ASSERT_EQ(work_txs.size(), 100);
  for (auto &tx : work_txs) {
    check_uniquifier(tx.tx.uniquifier);
    auto handler = make_new_system_call_handler(tx, *runtime_context_);
    auto invoke_res = handler.root_invoke();
    ASSERT_TRUE(invoke_res.is_root_success());
    auto res = handler.try_commit_transaction();
    ASSERT_TRUE(res == ExecResult::ExecResultOK);
    if (!gas_used) {
      gas_used = invoke_res.gas_consumed_;
    }
    ASSERT_EQ(invoke_res.gas_consumed_, gas_used.value());
    ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK,
                                    ProposedTransaction{.signed_tx = tx})());
  }
}

TEST_F(ERC20ExperimentTest, BasicAuthGenesisIsStableAcrossRegeneration)
{
  ERC20PaymentsConfig config{.num_addresses = 10};

  auto state1 = std::make_shared<ERC20BasicAuthExperimentState>(config);
  auto state2 = std::make_shared<ERC20BasicAuthExperimentState>(config);

  auto result1 = state1->generate_experiment()();
  auto result2 = state2->generate_experiment()();

  ASSERT_TRUE(result1.has_value());
  ASSERT_TRUE(result2.has_value());
  EXPECT_EQ(result1.value().genesis_configuration,
            result2.value().genesis_configuration);
  EXPECT_EQ(result1.value().initialization_txs,
            result2.value().initialization_txs);
}

TEST_F(ERC20ExperimentTest, DISABLED_ManyAccounts)
{
  auto state = std::make_shared<ERC20ExperimentState>(ERC20PaymentsConfig{
      .num_addresses = 1000,
  });
  auto result = state->generate_experiment()();
  ASSERT_TRUE(result.has_value());

  run_genesis_initialization(result.value().genesis_configuration);
  check_initial_balance();

  for (auto &init_block : result.value().initialization_txs) {
    for (auto &tx : init_block) {
      check_uniquifier(tx.tx.uniquifier);
      auto handler = make_new_system_call_handler(tx, *runtime_context_);
      auto invoke_res = handler.root_invoke();
      ASSERT_TRUE(invoke_res.is_root_success());
      auto res = handler.try_commit_transaction();
      ASSERT_TRUE(res == ExecResult::ExecResultOK);
      ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK,
                                      ProposedTransaction{.signed_tx = tx})());
    }

    advance_block();
  }

  std::optional<GasAmount> gas_used;

  LoadgenStatus<LoadgenExtension> work_txs_raw =
      state->extend_experiment(10000, 10)();
  ASSERT_TRUE(work_txs_raw.has_value());

  SignedTransactionBatch work_txs;
  parallel_batch_to_t(work_txs_raw.value().work_txs, work_txs)();
  for (auto &tx : work_txs) {
    check_uniquifier(tx.tx.uniquifier);
    auto handler = make_new_system_call_handler(tx, *runtime_context_);
    auto invoke_res = handler.root_invoke();
    ASSERT_TRUE(invoke_res.is_root_success());
    auto res = handler.try_commit_transaction();
    ASSERT_TRUE(res == ExecResult::ExecResultOK);
    if (!gas_used) {
      gas_used = invoke_res.gas_consumed_;
    }
    ASSERT_EQ(invoke_res.gas_consumed_, gas_used.value());
    ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK,
                                    ProposedTransaction{.signed_tx = tx})());
  }
}

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  return hogd::run_hog_exec_tests(argc, argv);
}
