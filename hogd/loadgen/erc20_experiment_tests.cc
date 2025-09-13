#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <optional>

#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/init.h"
#include "hogd/loadgen/erc20_experiment.h"
#include "loadgen.hh"
#include "rpc_types.hh"
#include "types.hh"

namespace hogd {

class ERC20ExperimentTest : public ::testing::TestWithParam<TestDB>,
                            public NativeContractTestFramework {
public:
  ERC20ExperimentTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam(),
          false)
  {}

protected:
  void SetUp() override { wasm_runtime_context_ = make_hogexec_context(); }

  std::shared_ptr<RuntimeContext> wasm_runtime_context_;
};

TEST_P(ERC20ExperimentTest, Basic)
{
  auto state = std::make_shared<ERC20ExperimentState>(ERC20PaymentsConfig{
      .num_accounts = 10,
  });
  auto result = state->generate_experiment();
  ASSERT_TRUE(result.has_value());

  run_genesis_initialization(result.value().genesis_block);

  for (auto &init_block : result.value().initialization_txs) {
    for (auto &tx : init_block) {
      auto handler = make_new_system_call_handler(tx, wasm_runtime_context_);
      auto invoke_res = handler.root_invoke();
      ASSERT_TRUE(invoke_res.is_root_success());
      auto res = handler.try_commit_transaction();
      ASSERT_TRUE(res.has_value());
      ASSERT_TRUE(*res);
    }

    advance_block();
  }

  std::optional<GasAmount> gas_used;

  auto work_txs = state->extend_experiment(100);
  ASSERT_TRUE(work_txs.has_value());
  ASSERT_EQ(work_txs.value().work_txs.size(), 100);

  for (auto &tx : work_txs.value().work_txs) {
    auto handler = make_new_system_call_handler(tx, wasm_runtime_context_);
    auto invoke_res = handler.root_invoke();
    ASSERT_TRUE(invoke_res.is_root_success());
    auto res = handler.try_commit_transaction();
    ASSERT_TRUE(res.has_value());
    ASSERT_TRUE(*res);
    if (!gas_used) {
      gas_used = invoke_res.gas_consumed_;
    }
    ASSERT_EQ(invoke_res.gas_consumed_, gas_used.value());
  }
}

TEST_P(ERC20ExperimentTest, DISABLED_ManyAccounts)
{
  auto state = std::make_shared<ERC20ExperimentState>(ERC20PaymentsConfig{
      .num_accounts = 1000,
  });
  auto result = state->generate_experiment();
  ASSERT_TRUE(result.has_value());

  run_genesis_initialization(result.value().genesis_block);

  for (auto &init_block : result.value().initialization_txs) {
    for (auto &tx : init_block) {
      auto handler = make_new_system_call_handler(tx, wasm_runtime_context_);
      auto invoke_res = handler.root_invoke();
      ASSERT_TRUE(invoke_res.is_root_success());
      auto res = handler.try_commit_transaction();
      ASSERT_TRUE(res.has_value());
      ASSERT_TRUE(*res);
    }

    advance_block();
  }

  std::optional<GasAmount> gas_used;

  auto work_txs = state->extend_experiment(10000);
  ASSERT_TRUE(work_txs.has_value());

  for (auto &tx : work_txs.value().work_txs) {
    auto handler = make_new_system_call_handler(tx, wasm_runtime_context_);
    auto invoke_res = handler.root_invoke();
    ASSERT_TRUE(invoke_res.is_root_success());
    auto res = handler.try_commit_transaction();
    ASSERT_TRUE(res.has_value());
    ASSERT_TRUE(*res);
    if (!gas_used) {
      gas_used = invoke_res.gas_consumed_;
    }
    ASSERT_EQ(invoke_res.gas_consumed_, gas_used.value());
  }
}

INSTANTIATE_TEST_SUITE_P(ERC20ExperimentTest, ERC20ExperimentTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
