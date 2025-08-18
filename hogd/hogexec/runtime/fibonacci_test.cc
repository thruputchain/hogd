#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "rpc_types.hh"
#include "test_contracts/fibonacci.h"
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
  void SetUp() override
  {
    wasm_runtime_context_ = make_hogexec_wasm_context(RuntimeEngine::kWasmi);
  }

  std::shared_ptr<RuntimeContext> wasm_runtime_context_;

  // An amount of gas that suffices for correct tests, but means that if there's
  // another calldata parsing error, we'll run out of gas and error out quickly
  // instead of trying to compute an absurdly high fibonacci.
  constexpr static GasAmount kSufficientGas = 10000;
};

// Simple test for sdk, to verify that calls actually happen.
TEST_P(FibonacciTest, FibonacciCalls)
{
  auto contract = test::load_test_contract("fibonacci.wasm");
  auto contract_addr = make_contract_address(0);
  write_contract(contract);
  write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  using fib_client_t = ContractTraitClient<Fibonacci>;
  fib_client_t client(contract_addr);

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation = client.make_invocation<"fibonacci">(
                                  kSufficientGas, 5U)}},
        wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }
}

TEST_P(FibonacciTest, FibonacciGasLimit)
{
  auto contract = test::load_test_contract("fibonacci.wasm");
  auto contract_addr = make_contract_address(0);
  write_contract(contract);
  write_deployment(contract_addr, sha256(*contract));
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  using fib_client_t = ContractTraitClient<Fibonacci>;
  fib_client_t client(contract_addr);

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        client.make_invocation<"slow_fibonacci">(2000, 20U)}},
        wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_FALSE(res.is_root_success());
    ASSERT_EQ(res.gas_consumed_, 2000);
  }
}

// Test whether the output of the fibonacci contract is correct.
TEST_P(FibonacciTest, FibonacciCorrectness)
{
  // deploy the fibonacci contract
  auto contract = test::load_test_contract("fibonacci.wasm");
  auto fib_contract_addr = make_contract_address(0);
  write_contract(contract);
  write_deployment(fib_contract_addr, sha256(*contract));
  setup_grant_storage_token(fib_contract_addr, 1'000'000'000);

  using fib_client_t = ContractTraitClient<Fibonacci>;

  NativeContractBuilder builder;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0,
      [fib_contract_addr](SystemCallHandler &handler, uint32_t input,
                          uint32_t expect) -> InvokeStatus<void> {
        fib_client_t client(fib_contract_addr);
        auto [decoded, _gas] =
            client.template invoke<"fibonacci">(handler, input);
        if (!decoded.has_value() || !decoded.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (decoded.value().value() != expect) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
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
  write_deployment(native_contract_addr, sha256(*native_contract_xdr));

  auto multiplexed_context = std::make_shared<MultiplexedContext>(
      std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
          {ContractType::kNative, native_runtime_context},
          {ContractType::kWasmi, wasm_runtime_context_}});

  fib_client_t client(fib_contract_addr);

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
                                        .value()}}},
        multiplexed_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
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
                                        .value()}}},
        multiplexed_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }
  // NOLINTEND(bugprone-unchecked-optional-access)
}

INSTANTIATE_TEST_SUITE_P(TestDBs, FibonacciTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
