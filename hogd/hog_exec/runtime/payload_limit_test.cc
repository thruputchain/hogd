#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "ffi_enums.hh"
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

// Small and mutually distinct, so a gate reading the wrong parameter, or the
// old fixed kMaxReturnBufferLen, fails rather than coincidentally passing.
constexpr uint32_t kMaxAuthTokens = 4;
constexpr uint32_t kMaxCalldataLen = 64;
constexpr uint32_t kMaxReturnBufferLenCfg = 128;

constexpr ContractAddress kCallerAddress =
    NativeContractTestFramework::make_contract_address(0);
constexpr ContractAddress kChildAddress =
    NativeContractTestFramework::make_contract_address(1);

} // namespace

// Each of the three payload bounds is refused before the work it would cost:
// the token list before the parse, the calldata before the copy, the return
// buffer before it is staged.
class PayloadLimitTest : public ::testing::Test,
                         public NativeContractTestFramework {
public:
  PayloadLimitTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb, true, std::nullopt, genesis_network_config())
  {}

protected:
  void SetUp() override
  {
    setup_grant_storage_token(kCallerAddress, replay_cache_cost_charged(1));
  }

  // Runs `caller` as the root invocation with `child` deployed alongside it.
  void run_pair(NativeContract::function_t child,
                NativeContract::function_t caller)
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
    fns_child.emplace(0, std::move(child));
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_caller;
    fns_caller.emplace(0, std::move(caller));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::make_unique<NativeContract>(
                                std::move(fns_caller)));
    contract_map.emplace(1, std::make_unique<NativeContract>(
                                std::move(fns_child)));
    auto runtime_context =
        std::make_unique<NativeContext>(std::move(contract_map));

    auto caller_xdr = make_native_contract(0);
    write_contract(caller_xdr);
    write_deployment(kCallerAddress, caller_xdr);
    auto child_xdr = make_native_contract(1);
    write_contract(child_xdr);
    write_deployment(kChildAddress, child_xdr);

    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{
                            .invoked_address = kCallerAddress,
                            .entrypoint = entrypoint_from_method_id(0),
                            .gas_limit = UINT64_MAX,
                            .calldata = {},
                        },
                    .uniquifier = make_new_uniquifier(),
                }},
        *runtime_context);
    auto res = handler.root_invoke()();
    ASSERT_TRUE(res.is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  static CopyableInvokeInputs plain_invoke()
  {
    return CopyableInvokeInputs{
        .entrypoint_id = 0,
        .gas_limit = UINT64_MAX,
        .flags = 0,
    };
  }

private:
  static NetworkConfig genesis_network_config()
  {
    NetworkConfig config = stock_network_config();
    NetworkConfigParams::set_param(config,
                                   ConfigParamID::kConfigParamMaxAuthTokens,
                                   kMaxAuthTokens);
    NetworkConfigParams::set_param(config,
                                   ConfigParamID::kConfigParamMaxCalldataLen,
                                   kMaxCalldataLen);
    NetworkConfigParams::set_param(
        config, ConfigParamID::kConfigParamMaxReturnBufferLen,
        kMaxReturnBufferLenCfg);
    return config;
  }
};

// The ids are unowned too, but the length gate answers first: it runs ahead of
// the parse the ownership check depends on.
TEST_F(PayloadLimitTest, InvokeOverAuthTokenLimitIsRejected)
{
  bool child_invoked = false;
  run_pair(
      [&](NativeSyscallContext &) -> x23::Async<InvokeStatus<void>> {
        child_invoked = true;
        ADD_FAILURE() << "an over-limit token list must not start a frame";
        co_return InvokeStatus<void>{};
      },
      [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        std::vector<uint32_t> token_ids(kMaxAuthTokens + 1);
        for (uint32_t i = 0; i < token_ids.size(); ++i) {
          token_ids[i] = i;
        }
        auto inputs = plain_invoke();
        auto res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{token_ids}, inputs,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(res.has_value());
        EXPECT_EQ(res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
        co_return InvokeStatus<void>{};
      });
  EXPECT_FALSE(child_invoked);
}

// One byte past the limit is refused before the calldata is copied, and no
// child frame runs.
TEST_F(PayloadLimitTest, InvokeOverCalldataLimitIsRejected)
{
  bool child_invoked = false;
  run_pair(
      [&](NativeSyscallContext &) -> x23::Async<InvokeStatus<void>> {
        child_invoked = true;
        ADD_FAILURE() << "over-limit calldata must not start a frame";
        co_return InvokeStatus<void>{};
      },
      [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        std::vector<std::byte> calldata(kMaxCalldataLen + 1, std::byte{0xAB});
        auto inputs = plain_invoke();
        auto res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{calldata},
            std::span<uint32_t const>{}, inputs, CopyableInvokeOutputs{});
        EXPECT_TRUE(res.has_value());
        EXPECT_EQ(res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
        co_return InvokeStatus<void>{};
      });
  EXPECT_FALSE(child_invoked);
}

// The limit is inclusive: calldata of exactly the stated length is admitted
// and the child runs. This is the boundary the gate above must not cross.
TEST_F(PayloadLimitTest, InvokeAtCalldataLimitIsAccepted)
{
  bool child_invoked = false;
  run_pair(
      [&](NativeSyscallContext &) -> x23::Async<InvokeStatus<void>> {
        child_invoked = true;
        co_return InvokeStatus<void>{};
      },
      [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        std::vector<std::byte> calldata(kMaxCalldataLen, std::byte{0xCD});
        auto inputs = plain_invoke();
        CopyableInvokeOutputs outputs{};
        auto res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{calldata},
            std::span<uint32_t const>{}, inputs, outputs);
        EXPECT_TRUE(res.has_value());
        EXPECT_EQ(res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(outputs.frame_status, InvocationFrameStatusCode::Return);
        co_return InvokeStatus<void>{};
      });
  EXPECT_TRUE(child_invoked);
}

// The Return gate reads the configured limit, not the old fixed constant: this
// buffer is well under kMaxReturnBufferLen's historical 1024 and still faults
// the frame, because the chain states a lower one.
TEST_F(PayloadLimitTest, ReturnOverConfiguredBufferLimitFaultsTheChild)
{
  run_pair(
      [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        std::vector<std::byte> buffer(kMaxReturnBufferLenCfg + 1,
                                      std::byte{0x11});
        auto res = co_await ctx.syscall<SyscallNo::kReturn>(
            std::span<const std::byte>{buffer}, false,
            std::span<uint32_t const>{});
        // Return does not come back on success; reaching here at all means the
        // host refused it, which it reports as a deterministic frame failure.
        EXPECT_FALSE(res.has_value());
        co_return InvokeError{InvokeErrorCode::Deterministic};
      },
      [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto inputs = plain_invoke();
        CopyableInvokeOutputs outputs{};
        auto res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{}, inputs, outputs);
        EXPECT_TRUE(res.has_value());
        EXPECT_EQ(res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(outputs.frame_status, InvocationFrameStatusCode::Fault);
        co_return InvokeStatus<void>{};
      });
}

// Inclusive on this side too: a buffer of exactly the stated length returns
// normally.
TEST_F(PayloadLimitTest, ReturnAtConfiguredBufferLimitIsAccepted)
{
  run_pair(
      [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        std::vector<std::byte> buffer(kMaxReturnBufferLenCfg, std::byte{0x22});
        auto res = co_await ctx.syscall<SyscallNo::kReturn>(
            std::span<const std::byte>{buffer}, false,
            std::span<uint32_t const>{});
        EXPECT_FALSE(res.has_value());
        co_return InvokeStatus<void>{};
      },
      [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto inputs = plain_invoke();
        CopyableInvokeOutputs outputs{};
        auto res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{}, inputs, outputs);
        EXPECT_TRUE(res.has_value());
        EXPECT_EQ(res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(outputs.frame_status, InvocationFrameStatusCode::Return);
        co_return InvokeStatus<void>{};
      });
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
