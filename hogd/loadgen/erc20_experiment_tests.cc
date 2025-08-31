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
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override { wasm_runtime_context_ = make_hogexec_context(); }

  std::shared_ptr<RuntimeContext> wasm_runtime_context_;
};

TEST_P(ERC20ExperimentTest, Basic)
{
  auto result = generate_erc20_payments_experiment(ERC20PaymentsConfig{
      .num_txs = 100,
      .num_accounts = 10,
  });

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

  for (auto &tx : result.value().work_txs) {
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

TEST_P(ERC20ExperimentTest, ManyAccounts)
{
  auto result = generate_erc20_payments_experiment(ERC20PaymentsConfig{
      .num_txs = 10000,
      .num_accounts = 1000,
  });

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

  for (auto &tx : result.value().work_txs) {
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
