#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "built_contracts/contract_loader.h"
#include "built_contracts/fibonacci_call.h"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class CrossCallTests : public ::testing::TestWithParam<TestDB>,
                       public NativeContractTestFramework {
public:
  CrossCallTests()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override
  {
    wasm_runtime_context_ = make_hogexec_wasm_context(RuntimeEngine::kWasmi);

    auto fibonacci_contract = hogd::load_contract("fibonacci.wasm");
    auto fibonacci_caller_contract = hogd::load_contract("fibonacci_call.wasm");

    write_contract(fibonacci_contract);
    write_contract(fibonacci_caller_contract);
    write_deployment(fibonacci_contract_address_, fibonacci_contract);
    write_deployment(fibonacci_caller_contract_address_,
                     fibonacci_caller_contract);

    setup_grant_storage_token(fibonacci_contract_address_, 1'000'000'000);
    setup_grant_storage_token(fibonacci_caller_contract_address_,
                              1'000'000'000);
  }

  std::shared_ptr<RuntimeContext> wasm_runtime_context_;

  const ContractAddress fibonacci_contract_address_ = make_contract_address(0);
  const ContractAddress fibonacci_caller_contract_address_ =
      make_contract_address(1);

  ContractTraitClient<fibonacci_call::FibonacciCaller>
      fibonacci_caller_client_ =
          ContractTraitClient<fibonacci_call::FibonacciCaller>(
              fibonacci_caller_contract_address_);

  void initialize_fibonacci_caller()
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        fibonacci_caller_client_.make_invocation<"initialize">(
                            UINT64_MAX, fibonacci_contract_address_)}},
        wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);

    advance_block();
  }
};

TEST_P(CrossCallTests, CrossCallNoGasLimit)
{

  initialize_fibonacci_caller();

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{.invocation =
                                fibonacci_caller_client_
                                    .make_invocation<"call_fibonacci">(
                                        UINT64_MAX, uint32_t{5}, UINT64_MAX)}},
        wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }

  {
    // try again with larger number
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{.invocation =
                                fibonacci_caller_client_
                                    .make_invocation<"call_fibonacci">(
                                        UINT64_MAX, uint32_t{20}, UINT64_MAX)}},
        wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }
}

TEST_P(CrossCallTests, LowGasCausesDeterministicRevert)
{
  initialize_fibonacci_caller();

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  fibonacci_caller_client_
                                      .make_invocation<"call_fibonacci">(
                                          UINT64_MAX, uint32_t{20},
                                          uint64_t{1000})}},
        wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_FALSE(res.is_root_success());
    ASSERT_EQ(res.result_.error().code(),
              InvokeErrorCode::kInvokeDeterministic);
  }
}

TEST_P(CrossCallTests, LowOverallGasPropagates)
{
  initialize_fibonacci_caller();

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  fibonacci_caller_client_
                                      .make_invocation<"call_fibonacci">(
                                          1000, uint32_t{20}, UINT64_MAX)}},
        wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_FALSE(res.is_root_success());
    ASSERT_EQ(res.gas_consumed_, 1000);
  }
}

TEST_P(CrossCallTests, ExpectOutOfGas)
{
  initialize_fibonacci_caller();

  uint64_t gas_consumed_1 = 0, gas_consumed_2 = 0;

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{.invocation =
                                fibonacci_caller_client_.make_invocation<
                                    "call_fibonacci_expect_out_of_gas">(
                                    UINT64_MAX, uint32_t{20}, uint64_t{1000})}},
        wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
    ASSERT_GE(res.gas_consumed_, 1000);
    gas_consumed_1 = res.gas_consumed_;
  }

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{.invocation =
                                fibonacci_caller_client_.make_invocation<
                                    "call_fibonacci_expect_out_of_gas">(
                                    UINT64_MAX, uint32_t{20}, uint64_t{1010})}},
        wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
    ASSERT_GE(res.gas_consumed_, 1010);
    gas_consumed_2 = res.gas_consumed_;
  }

  ASSERT_EQ(gas_consumed_1 + 10, gas_consumed_2);
}

INSTANTIATE_TEST_SUITE_P(TestDBs, CrossCallTests,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
