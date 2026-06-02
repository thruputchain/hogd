#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <set>

#include "exec_result.hh"
#include "hogd/base/xdr_util.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/init.h"
#include "hogd/loadgen/erc20_basic_auth_experiment.h"
#include "hogd/loadgen/erc20_experiment.h"
#include "hogd/loadgen/status.h"
#include "loadgen.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class ERC20ExperimentTest : public ::testing::Test,
                            public NativeContractTestFramework {
public:
  ERC20ExperimentTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB, false)
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

  void SetUp() override { runtime_context_ = make_hogexec_context(); }
};

TEST_F(ERC20ExperimentTest, Basic)
{
  auto state = std::make_shared<ERC20ExperimentState>(ERC20PaymentsConfig{
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
      ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
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
    ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
  }
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
      ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
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
    ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
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
      ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
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
    ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
  }
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
