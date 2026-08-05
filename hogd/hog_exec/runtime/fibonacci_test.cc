#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

#include "built_contracts/contract_loader.h"
#include "built_contracts/fibonacci.h"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/serialize_calldata.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class FibonacciTest : public ::testing::TestWithParam<TestDB>,
                      public NativeContractTestFramework {
public:
  FibonacciTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  std::unique_ptr<RuntimeContext> wasm_runtime_context_;

  static std::unique_ptr<RuntimeContext> make_context()
  {
    return make_hog_exec_wasm_context(RuntimeEngine::kWasmi);
  };

  std::unique_ptr<RuntimeContext> runtime_context_;

  using fib_client_t = ContractTraitClient<fibonacci::Fibonacci>;

  // An amount of gas that suffices for correct tests, but means that if there's
  // another calldata parsing error, we'll run out of gas and error out quickly
  // instead of trying to compute an absurdly high fibonacci.
  constexpr static GasAmount kSufficientGas = 10000;

  void SetUp() override { runtime_context_ = make_context(); }
};

// Simple test for sdk, to verify that calls actually happen.
TEST_P(FibonacciTest, FibonacciCalls)
{
  auto contract = hogd::load_contract("fibonacci.wasm");
  auto contract_addr = make_contract_address(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  fib_client_t client(contract_addr);

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{.invocation = client.make_invocation<u8"fibonacci">(
                                kSufficientGas, 5U),
                            .uniquifier = make_new_uniquifier()}},
        *runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
}

TEST_P(FibonacciTest, FibonacciGasLimit)
{
  auto contract = hogd::load_contract("fibonacci.wasm");
  auto contract_addr = make_contract_address(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  fib_client_t client(contract_addr);

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{.invocation =
                                client.make_invocation<u8"slow_fibonacci">(2000,
                                                                           20U),
                            .uniquifier = make_new_uniquifier()}},
        *runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_FALSE(res.is_root_success());
    ASSERT_EQ(res.gas_consumed_, 2000);
  }
}

// Test whether the output of the fibonacci contract is correct.
TEST_P(FibonacciTest, FibonacciCorrectness)
{
  // deploy the fibonacci contract
  auto contract = hogd::load_contract("fibonacci.wasm");
  constexpr static ContractAddress kFibContractAddr = make_contract_address(0);
  write_contract(contract);
  write_deployment(kFibContractAddr, contract);
  setup_grant_storage_token(kFibContractAddr, 1'000'000'000);

  NativeContractBuilder builder;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0,
      [](NativeSyscallContext &ctx, uint32_t input,
         uint32_t expect) -> InvokeStatus<void> {
        fib_client_t client(kFibContractAddr);
        auto decoded = client.template invoke_opt<u8"fibonacci">(ctx, input);
        if (!decoded.has_value() || !decoded.value().has_value() ||
            !decoded.value().value().has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (decoded.value().value().value() != expect) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  auto native_contract = builder.build();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>>
      native_contract_map;
  native_contract_map.emplace(0, std::move(native_contract));
  auto native_runtime_context =
      std::make_shared<NativeContext>(std::move(native_contract_map));

  // deploy the native contract
  auto native_contract_xdr = make_native_contract(0);
  auto native_contract_addr = make_contract_address(1);
  write_contract(native_contract_xdr);
  write_deployment(native_contract_addr, native_contract_xdr);
  // The two commits below each consume a replay nonce, charged to the invoked
  // address. They run in the same block, so each consume is priced against the
  // committed snapshot (no in-block discount); grant one nonce's cost per
  // commit.
  setup_grant_storage_token(native_contract_addr,
                            replay_cache_cost_charged(1) * 2);

  auto make_multiplexed_context = [&]() -> std::unique_ptr<MultiplexedContext> {
    auto wasm_context = make_context();

    return std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kNative, {native_runtime_context.get(), false}},
            {ContractType::kWasmi, {wasm_context.release(), true}}});
  };

  auto context = make_multiplexed_context();

  fib_client_t client(kFibContractAddr);

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  {
    // fibonacci(10) = 55
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{
                            .invoked_address = native_contract_addr,
                            .entrypoint = entrypoint_from_method_id(0),
                            .gas_limit = UINT64_MAX,
                            .calldata =
                                Serializer<NativeContractBuilder::
                                               kDefaultSerializationFormat>::
                                    serialize<uint32_t, uint32_t>(10U, 55U)
                                        .value()},
                    .uniquifier = make_new_uniquifier()}},
        *context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }

  {
    // fibonacci(40) = 102334155 (0x6197ECB)
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{
                            .invoked_address = native_contract_addr,
                            .entrypoint = entrypoint_from_method_id(0),
                            .gas_limit = UINT64_MAX,
                            .calldata =
                                Serializer<NativeContractBuilder::
                                               kDefaultSerializationFormat>::
                                    serialize<uint32_t, uint32_t>(40U,
                                                                  102334155U)
                                        .value()},
                    .uniquifier = make_new_uniquifier()}},
        *context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
  // NOLINTEND(bugprone-unchecked-optional-access)
}

INSTANTIATE_TEST_SUITE_P(TestDBs, FibonacciTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

} // namespace hogd
