#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hog_exec_constants.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "network_config.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

namespace {

// Far below the protocol default, so a test passing here shows the configured
// value is what is enforced rather than the constant.
constexpr uint32_t kBitBudget = 4;
static_assert(kBitBudget < kDefaultMaxNondeterministicBits);

// Decisions each driver attempts: past the budget by this many.
constexpr uint32_t kOverBudget = 3;

constexpr ContractAddress kAddressNoop =
    NativeContractTestFramework::make_contract_address(0);
constexpr ContractAddress kAddressDriver =
    NativeContractTestFramework::make_contract_address(1);

constexpr StorageKeyContractData kEmptyQueueKey =
    NativeContractTestFramework::make_storage_key("empty_rcq");

// Driver: kBitBudget + kOverBudget TryInvokes of the noop contract. The first
// kBitBudget commit; every one past the budget is refused without the syscall
// erroring, and the transaction still succeeds.
x23::Async<InvokeStatus<void>>
try_invoke_past_budget(NativeSyscallContext &ctx)
{
  uint32_t committed = 0;
  for (uint32_t i = 0; i < kBitBudget + kOverBudget; i++) {
    CopyableInvokeInputs inputs{
        .entrypoint_id = 0,
        .gas_limit = UINT64_MAX,
        .flags = std::to_underlying(InvokeFlag::TryInvoke),
    };
    CopyableInvokeOutputs outputs{};
    auto r =
        co_await ctx.syscall<SyscallNo::kInvoke>(kAddressNoop,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 outputs);
    if (!r.has_value() ||
        r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (outputs.frame_status == InvocationFrameStatusCode::Return) {
      committed++;
    }
  }
  if (committed != kBitBudget) {
    ADD_FAILURE() << "expected " << kBitBudget << " committed TryInvokes, got "
                  << committed;
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  co_return InvokeStatus<void>{};
}

// Driver: kBitBudget + kOverBudget peeks of an empty queue. Each finds no
// entry; the ones past the budget do so without recording a bit.
x23::Async<InvokeStatus<void>>
peek_empty_queue_past_budget(NativeSyscallContext &ctx)
{
  for (uint32_t i = 0; i < kBitBudget + kOverBudget; i++) {
    RuntimeMemoryLength len = 0;
    auto r = co_await ctx.syscall<SyscallNo::kRetrocausalQueuePeekLen>(
        kEmptyQueueKey, len);
    if (!r.has_value() ||
        r.value() !=
            SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry) {
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
  }
  co_return InvokeStatus<void>{};
}

} // namespace

// kConfigParamMaxNondeterministicBits bounds the proposal sidecar without
// failing the transaction that reaches it: decisions past it take a fixed
// outcome and record nothing, and replay agrees (#2902).
class NondeterministicBudgetTest : public ::testing::TestWithParam<TestDB>,
                                   public NativeContractTestFramework {
protected:
  NondeterministicBudgetTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam(),
          true, std::nullopt, lowered_network_config())
  {}

  void SetUp() override
  {
    for (auto const &addr : {kAddressNoop, kAddressDriver}) {
      setup_grant_storage_token(addr, 1'000'000'000);
    }
  }

  // Deploys a noop contract at kAddressNoop and `driver` at kAddressDriver.
  std::unique_ptr<NativeContext> deploy(NativeContract::function_t driver)
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_noop;
    fns_noop.emplace(
        0, [](NativeSyscallContext &) -> x23::Async<InvokeStatus<void>> {
          co_return InvokeStatus<void>{};
        });
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_driver;
    fns_driver.emplace(0, std::move(driver));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0,
                         std::make_unique<NativeContract>(std::move(fns_noop)));
    contract_map.emplace(1, std::make_unique<NativeContract>(
                                std::move(fns_driver)));

    ContractAddress const addrs[] = {kAddressNoop, kAddressDriver};
    for (int i = 0; i < 2; ++i) {
      auto c = make_native_contract(i);
      write_contract(c);
      write_deployment(addrs[i], c);
    }
    return std::make_unique<NativeContext>(std::move(contract_map));
  }

  Transaction make_driver_tx()
  {
    return Transaction{.invocation =
                           Invocation{.invoked_address = kAddressDriver,
                                      .entrypoint =
                                          entrypoint_from_method_id(0),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = {}},
                       .uniquifier = make_new_uniquifier()};
  }

  // Proposes `tx`, then replays the recorded sidecar in validation mode on the
  // same pre-block state. Both must succeed; returns the sidecar.
  NondeterministicProposalData propose_then_validate(Transaction const &tx,
                                                     RuntimeContext &ctx)
  {
    NondeterministicProposalData proposal_data;
    {
      auto handler = make_new_system_call_handler(tx, ctx);
      EXPECT_TRUE(handler.root_invoke()().is_root_success());
      proposal_data = handler.get_proposal_data();
      EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
    }

    discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

    auto handler =
        make_new_system_call_handler_validation(tx, ctx, proposal_data);
    EXPECT_TRUE(handler.root_invoke()().is_root_success());
    EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
    return proposal_data;
  }

private:
  static NetworkConfig lowered_network_config()
  {
    NetworkConfig config = stock_network_config();
    NetworkConfigParams::set_param(
        config, ConfigParamID::kConfigParamMaxNondeterministicBits, kBitBudget);
    return config;
  }
};

TEST_P(NondeterministicBudgetTest, TryInvokePastBudgetIsRefusedAndReplays)
{
  auto ctx = deploy(try_invoke_past_budget);
  auto proposal_data = propose_then_validate(make_driver_tx(), *ctx);
  EXPECT_EQ(proposal_data.nondeterministic_bit_count, kBitBudget);
}

TEST_P(NondeterministicBudgetTest, RcqPeekPastBudgetReadsEmptyAndReplays)
{
  auto ctx = deploy(peek_empty_queue_past_budget);
  auto proposal_data = propose_then_validate(make_driver_tx(), *ctx);
  EXPECT_EQ(proposal_data.nondeterministic_bit_count, kBitBudget);
}

// A sidecar stating more bits than the budget is rejected before the body
// runs, however it would have replayed.
TEST_P(NondeterministicBudgetTest, OversizedSidecarRejectedBeforeExecution)
{
  static bool driver_ran = false;
  driver_ran = false;
  auto ctx =
      deploy([](NativeSyscallContext &) -> x23::Async<InvokeStatus<void>> {
        driver_ran = true;
        co_return InvokeStatus<void>{};
      });

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  NondeterministicProposalData oversized;
  oversized.nondeterministic_bit_count = kBitBudget + 1;
  oversized.nondeterministic_bits.resize((kBitBudget + 1 + 7) / 8, 0);

  auto handler = make_new_system_call_handler_validation(make_driver_tx(), *ctx,
                                                         oversized);
  EXPECT_FALSE(handler.root_invoke()().is_root_success());
  EXPECT_FALSE(driver_ran);
  ASSERT_TRUE(handler.abort_transaction()().has_value());
}

INSTANTIATE_TEST_SUITE_P(TestDBs, NondeterministicBudgetTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
