#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "built_contracts/contract_loader.h"
#include "built_contracts/fibonacci.h"
#include "built_contracts/fibonacci_call.h"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/rv32_contract.h"
#include "hogd/hogexec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// End-to-end tests for the threaded RV32 runtime: load a real compiled contract
// artifact (.rv32a) as a kRv32 contract, resolve + translate it through
// Rv32Context, and invoke it through Rv32Runtime. Mirrors the wasm-side
// fibonacci tests (fibonacci_test.cc), but exercises the RV32 path.
// NOLINTNEXTLINE(misc-use-internal-linkage)
class Rv32FibonacciTest : public ::testing::TestWithParam<TestDB>,
                          public NativeContractTestFramework {
public:
  Rv32FibonacciTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  Rv32Context runtime_context_;

  using fib_client_t = ContractTraitClient<fibonacci::Fibonacci>;

  // Generous: enough to compute a small fibonacci and still fail fast (rather
  // than spin) if calldata parsing is wrong.
  constexpr static GasAmount kSufficientGas = 10'000'000;

  // Deploys a compiled .rv32a artifact as a kRv32 contract at the given
  // address seed, and returns its address.
  ContractAddress deploy_rv32(std::string_view artifact, uint64_t addr_seed)
  {
    auto contract = std::make_shared<Contract>(ContractType::kRv32);
    contract->rv32_contract() = load_contract_blob(artifact);
    auto addr = make_contract_address(addr_seed);
    write_contract(contract);
    write_deployment(addr, contract);
    setup_grant_storage_token(addr, 1'000'000'000);
    return addr;
  }
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// Smoke: the RV32 fibonacci contract loads, dispatches, runs, and returns
// success through Rv32Context/Rv32Runtime.
TEST_P(Rv32FibonacciTest, FibonacciCalls)
{
  auto contract_addr = deploy_rv32("fibonacci.rv32a", 0);
  fib_client_t client(contract_addr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation = client.make_invocation<"fibonacci">(
                                kSufficientGas, 5U),
                            .uniquifier = make_new_uniquifier()}},
      runtime_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// The deployment back-pointer exposes the eagerly-built rv32 translation,
// so the engine reaches a ready-to-instantiate image with no extra lookup.
TEST_P(Rv32FibonacciTest, DeploymentBackPointerCarriesEagerTranslation)
{
  auto contract_addr = deploy_rv32("fibonacci.rv32a", 0);

  auto cache = get_storage_snapshot_cache();
  auto deployed = cache->try_get_deployed_contract(contract_addr);
  ASSERT_TRUE(deployed.has_value());
  ASSERT_NE(deployed.value(), nullptr);
  ASSERT_NE(deployed.value()->entry, nullptr);
  EXPECT_EQ(deployed.value()->hash.contract_type, ContractType::kRv32);
  EXPECT_TRUE(deployed.value()->entry->translated.has_value());
  // Back-pointer aliases the contract-cache entry the hash lookup returns.
  auto by_hash = cache->try_get_contract_by_hash(deployed.value()->hash.hash);
  ASSERT_TRUE(by_hash.has_value());
  EXPECT_EQ(&deployed.value()->entry->contract, by_hash.value());
}

// Verifies the actual returned value: fibonacci(10) == 55, decoded from the
// root frame's return buffer (CXS-serialized u32).
TEST_P(Rv32FibonacciTest, FibonacciReturnsCorrectValue)
{
  auto contract_addr = deploy_rv32("fibonacci.rv32a", 0);
  fib_client_t client(contract_addr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation = client.make_invocation<"fibonacci">(
                                kSufficientGas, 10U),
                            .uniquifier = make_new_uniquifier()}},
      runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());

  auto output = handler.get_most_recent_frame_output();
  auto decoded = Serializer<SerializationFormat::kCxs>::deserialize<uint32_t>(
      std::as_bytes(std::span(output.return_buffer())));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, 55U);

  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

// slow_fibonacci is exponential recursion; with a small gas limit it traps
// out-of-gas, so the root invocation fails and the whole limit is consumed.
TEST_P(Rv32FibonacciTest, SlowFibonacciOutOfGas)
{
  auto contract_addr = deploy_rv32("fibonacci.rv32a", 0);
  fib_client_t client(contract_addr);

  constexpr GasAmount kGasLimit = 2000;
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                client.make_invocation<"slow_fibonacci">(
                                    kGasLimit, 20U),
                            .uniquifier = make_new_uniquifier()}},
      runtime_context_);
  auto res = handler.root_invoke();
  ASSERT_FALSE(res.is_root_success());
  ASSERT_EQ(res.gas_consumed_, kGasLimit);
}

// Cross-call where the inner fibonacci call is given too little gas. The caller
// uses the non-try client, so the inner out-of-gas propagates as a fault and
// the whole transaction fails.
TEST_P(Rv32FibonacciTest, CrossCallLowInnerGasReverts)
{
  auto fibonacci_addr = deploy_rv32("fibonacci.rv32a", 0);
  auto caller_addr = deploy_rv32("fibonacci_call.rv32a", 1);
  ContractTraitClient<fibonacci_call::FibonacciCaller> caller(caller_addr);

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{.invocation = caller.make_invocation<"initialize">(
                                UINT64_MAX, fibonacci_addr),
                            .uniquifier = make_new_uniquifier()}},
        runtime_context_);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
    advance_block();
  }

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  caller.make_invocation<"call_fibonacci">(
                                      UINT64_MAX, uint32_t{20}, uint64_t{1000}),
                              .uniquifier = make_new_uniquifier()}},
        runtime_context_);
    ASSERT_FALSE(handler.root_invoke().is_root_success());
  }
}

// Cross-call where the caller *expects* the inner call to run out of gas and
// handles it (try client), so the outer transaction still succeeds.
TEST_P(Rv32FibonacciTest, CrossCallExpectOutOfGas)
{
  auto fibonacci_addr = deploy_rv32("fibonacci.rv32a", 0);
  auto caller_addr = deploy_rv32("fibonacci_call.rv32a", 1);
  ContractTraitClient<fibonacci_call::FibonacciCaller> caller(caller_addr);

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{.invocation = caller.make_invocation<"initialize">(
                                UINT64_MAX, fibonacci_addr),
                            .uniquifier = make_new_uniquifier()}},
        runtime_context_);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
    advance_block();
  }

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation = caller.make_invocation<
                                  "call_fibonacci_expect_out_of_gas">(
                                  UINT64_MAX, uint32_t{20}, uint64_t{1000}),
                              .uniquifier = make_new_uniquifier()}},
        runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
    ASSERT_GE(res.gas_consumed_, 1000U);
  }
}

// Cross-call: an RV32 "caller" contract invokes a separate RV32 fibonacci
// contract. This re-enters Rv32Context::make_new_runtime for the inner call,
// exercising nested RV32 invocations through the syscall/ecall bridge.
TEST_P(Rv32FibonacciTest, CrossCallFibonacci)
{
  auto fibonacci_addr = deploy_rv32("fibonacci.rv32a", 0);
  auto caller_addr = deploy_rv32("fibonacci_call.rv32a", 1);

  ContractTraitClient<fibonacci_call::FibonacciCaller> caller(caller_addr);

  // initialize() stores the fibonacci contract address in the caller's state;
  // it writes state, so commit and advance to a new block before calling.
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{.invocation = caller.make_invocation<"initialize">(
                                UINT64_MAX, fibonacci_addr),
                            .uniquifier = make_new_uniquifier()}},
        runtime_context_);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
    advance_block();
  }

  // call_fibonacci() cross-calls fibonacci(5) on the stored address.
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  caller.make_invocation<"call_fibonacci">(
                                      UINT64_MAX, uint32_t{5}, UINT64_MAX),
                              .uniquifier = make_new_uniquifier()}},
        runtime_context_);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }
}

TEST(Rv32ContractTest, VerifyRejectsRunGasOverflow)
{
  constexpr size_t kOverflowingDivCount =
      (0xFFFFU / rv32::kDivInstructionGas) + 1;
  const std::vector<uint32_t> words(
      kOverflowingDivCount,
      rv32::test_program::op(3, 1, 2, 4, 0x01)); // div x3,x1,x2
  const std::vector<uint8_t> initial_memory{0};
  const std::vector<uint8_t> artifact =
      rv32::test_artifact::build_artifact(words, initial_memory);

  ContractBlob blob;
  blob.data.append_range(artifact);
  EXPECT_FALSE(Rv32Contract::create(blob).has_value());

  Contract contract(ContractType::kRv32);
  contract.rv32_contract() = std::move(blob);
  Rv32Context context;
  EXPECT_EQ(context.verify_contract(contract), InvokeErrorCode::Deterministic);
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, Rv32FibonacciTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
