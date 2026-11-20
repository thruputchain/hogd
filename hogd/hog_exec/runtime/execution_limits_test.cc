#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

#include "auth_token.hh"
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
#include "x23/serialize.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

namespace {

// Far below the protocol defaults, so a test passing here shows the
// configured value is what is enforced rather than the constant.
constexpr uint32_t kSavepointDepth = 4;
constexpr uint32_t kAuthTokenDepth = 2;
constexpr GasAmount kTxGas = 100'000'000;
static_assert(kSavepointDepth < kMaxSavepointDepth);
static_assert(kAuthTokenDepth < kAuthTokenMaxDepth);
static_assert(kTxGas < kDefaultMaxTxGas);

constexpr ContractAddress kAddressNoop =
    NativeContractTestFramework::make_contract_address(0);
constexpr ContractAddress kAddressDriver =
    NativeContractTestFramework::make_contract_address(1);

// One savepoint slot is reserved for the root commit, so the configured depth
// admits exactly this many child TryInvokes.
constexpr uint32_t kChildCap = kSavepointDepth - 1;

// A chain of `depth` nested AuthTokenNodes.
AuthTokenNode
make_token_tree(ContractAddress const &consumer, uint32_t depth)
{
  AuthTokenNode node{.consumer_address = consumer,
                     .payload = x23::serialize(depth),
                     .child_nodes = {}};
  for (uint32_t level = depth; level > 1; --level) {
    node = AuthTokenNode{.consumer_address = consumer,
                         .payload = x23::serialize(level - uint32_t{1}),
                         .child_nodes = {node}};
  }
  return node;
}

} // namespace

// The execution limits a chain states at genesis are what the executor
// enforces: these run under a configuration far below the protocol defaults
// and pin that the configured value, not the constant, is the boundary.
class ExecutionLimitsTest : public ::testing::Test,
                            public NativeContractTestFramework {
protected:
  ExecutionLimitsTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, true,
          std::nullopt, lowered_network_config())
  {}

  void SetUp() override
  {
    for (auto const &addr : {kAddressNoop, kAddressDriver}) {
      setup_grant_storage_token(addr, 1'000'000'000);
    }
  }

  // Deploys `noop` at kAddressNoop and `driver` at kAddressDriver, invokes the
  // driver as the root with a budget of `gas_limit`, and returns the root's
  // result.
  InvokeResult run_driver_with_budget(NativeContract::function_t driver,
                                      GasAmount gas_limit)
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
    auto context = std::make_unique<NativeContext>(std::move(contract_map));

    ContractAddress const addrs[] = {kAddressNoop, kAddressDriver};
    for (int i = 0; i < 2; ++i) {
      auto c = make_native_contract(i);
      write_contract(c);
      write_deployment(addrs[i], c);
    }

    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address = kAddressDriver,
                                             .entrypoint =
                                                 entrypoint_from_method_id(0),
                                             .gas_limit = gas_limit,
                                             .calldata = {}},
                              .uniquifier = make_new_uniquifier()}},
        *context);
    return handler.root_invoke()();
  }

  // As run_driver_with_budget, at the full budget the chain permits; reports
  // whether the root succeeded.
  bool run_driver(NativeContract::function_t driver)
  {
    return run_driver_with_budget(std::move(driver), kTxGas).is_root_success();
  }

private:
  static NetworkConfig lowered_network_config()
  {
    NetworkConfig config = stock_network_config();
    NetworkConfigParams::set_param(config,
                                   ConfigParamID::kConfigParamMaxSavepointDepth,
                                   kSavepointDepth);
    NetworkConfigParams::set_param(config,
                                   ConfigParamID::kConfigParamMaxAuthTokenDepth,
                                   kAuthTokenDepth);
    NetworkConfigParams::set_param(config, ConfigParamID::kConfigParamMaxTxGas,
                                   kTxGas);
    return config;
  }
};

// One savepoint slot is reserved for the root commit, so the configured
// depth admits exactly depth - 1 child TryInvokes; the one after fails
// deterministically without the syscall itself erroring.
TEST_F(ExecutionLimitsTest, TryInvokeStopsAtConfiguredSavepointDepth)
{
  const bool ok = run_driver(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        uint32_t successes = 0;
        for (uint32_t i = 0; i < kChildCap + 1; i++) {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          CopyableInvokeOutputs outputs{};
          auto r = co_await ctx.syscall<SyscallNo::kInvoke>(
              kAddressNoop, std::span<const std::byte>{}, std::span<uint32_t>{},
              inputs, outputs);
          if (!r.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r.value() == SyscallErrorCode::kSyscallErrorCodeNone &&
              outputs.frame_status == InvocationFrameStatusCode::Return) {
            successes++;
          }
        }
        if (successes != kChildCap) {
          ADD_FAILURE() << "expected " << kChildCap
                        << " committed TryInvokes, got " << successes;
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  EXPECT_TRUE(ok);
}

// A token at the configured depth mints; one level deeper is refused as
// invalid input, though the protocol default would have admitted it.
TEST_F(ExecutionLimitsTest, MintAuthTokenStopsAtConfiguredDepth)
{
  const bool ok = run_driver([](NativeSyscallContext &ctx)
                                 -> x23::Async<InvokeStatus<void>> {
    for (auto const &[depth, expected] :
         {std::pair{kAuthTokenDepth, SyscallErrorCode::kSyscallErrorCodeNone},
          std::pair{kAuthTokenDepth + 1,
                    SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput}}) {
      auto serialized = x23::serialize(make_token_tree(kAddressDriver, depth));
      uint32_t token_id = 0;
      auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
          BlockHeight{1000},
          std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                         serialized.data()),
                                     serialized.size()},
          token_id);
      if (!mint_res.has_value() || mint_res.value() != expected) {
        ADD_FAILURE() << "depth " << depth << ": unexpected mint result";
        co_return InvokeError{InvokeErrorCode::Deterministic};
      }
    }
    co_return InvokeStatus<void>{};
  });
  EXPECT_TRUE(ok);
}

// A budget at the configured bound runs.
TEST_F(ExecutionLimitsTest, RootBudgetAtConfiguredTxGasRuns)
{
  auto ran = std::make_shared<bool>(false);
  EXPECT_TRUE(run_driver(
      [ran](NativeSyscallContext &) -> x23::Async<InvokeStatus<void>> {
        *ran = true;
        co_return InvokeStatus<void>{};
      }));
  EXPECT_TRUE(*ran);
}

// One gas over the bound is refused before the body runs, as a validation
// failure rather than a clamped budget, though the protocol default would have
// admitted it.
TEST_F(ExecutionLimitsTest, RootBudgetOverConfiguredTxGasIsRefused)
{
  auto ran = std::make_shared<bool>(false);
  const InvokeResult over = run_driver_with_budget(
      [ran](NativeSyscallContext &) -> x23::Async<InvokeStatus<void>> {
        *ran = true;
        co_return InvokeStatus<void>{};
      },
      kTxGas + 1);
  ASSERT_FALSE(over.is_root_success());
  EXPECT_EQ(over.result_.error().code(), InvokeErrorCode::ValidationFailure);
  EXPECT_EQ(over.gas_consumed_, 0U);
  EXPECT_FALSE(*ran);
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
