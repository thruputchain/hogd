#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
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

constexpr uint32_t kMaxCrossInvokeDepth = 3;
// One contract longer than the limit admits, so the last one is deployed and
// invoked but must never be entered.
constexpr int32_t kChainLength = kMaxCrossInvokeDepth + 1;

} // namespace

// kConfigParamMaxCrossInvokeDepth bounds how many invocation frames one
// transaction may hold open. It is read off the DbTip, so this covers both the
// enforcement and the parameter reaching the executor as genesis set it.
class CrossInvokeDepthTest : public ::testing::Test,
                             public NativeContractTestFramework {
public:
  CrossInvokeDepthTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb, true, std::nullopt, genesis_network_config())
  {}

private:
  static NetworkConfig genesis_network_config()
  {
    NetworkConfig config = stock_network_config();
    config.max_cross_invoke_depth = kMaxCrossInvokeDepth;
    return config;
  }
};

TEST_F(CrossInvokeDepthTest, GenesisValueBoundsOpenFrames)
{
  // A chain of distinct contracts rather than one self-call: reentrance would
  // reject a self-invoke before the depth check is reached.
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  for (int32_t i = 0; i < kChainLength; i++) {
    const bool is_last = i == kChainLength - 1;
    // Contract i runs with i + 1 frames open (the root frame plus one per
    // contract entered), so contract kMaxCrossInvokeDepth - 1 is the first
    // whose invoke would exceed the limit.
    const bool expect_refused =
        i == static_cast<int32_t>(kMaxCrossInvokeDepth) - 1;
    const ContractAddress next = make_contract_address(i + 1);

    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0,
                [is_last, expect_refused, next](NativeSyscallContext &ctx)
                    -> x23::Async<InvokeStatus<void>> {
                  if (is_last) {
                    ADD_FAILURE()
                        << "the deepest contract must never be entered";
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  CopyableInvokeOutputs outputs{};
                  CopyableInvokeInputs inputs{
                      .entrypoint_id = 0,
                      .gas_limit = UINT64_MAX,
                      .flags = std::to_underlying(InvokeFlag::None),
                  };
                  auto r = co_await ctx.syscall<SyscallNo::kInvoke>(
                      next, std::span<const std::byte>{}, std::span<uint32_t>{},
                      inputs, outputs);
                  if (!r.has_value() ||
                      r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE() << "Invoke syscall failed outright";
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  // Exceeding the limit is reported as a faulted child frame,
                  // so the caller keeps running and the transaction still
                  // commits.
                  const auto expected = expect_refused
                                            ? InvocationFrameStatusCode::Fault
                                            : InvocationFrameStatusCode::Return;
                  if (outputs.frame_status != expected) {
                    ADD_FAILURE() << "unexpected child frame status";
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  co_return InvokeStatus<void>{};
                });
    contract_map.emplace(i, std::make_unique<NativeContract>(std::move(fns)));
  }

  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));
  for (int32_t i = 0; i < kChainLength; i++) {
    auto contract_obj = make_native_contract(i);
    write_contract(contract_obj);
    write_deployment(make_contract_address(i), contract_obj);
  }

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = make_contract_address(0),
                                    .entrypoint = entrypoint_from_method_id(0),
                                    .gas_limit = UINT64_MAX,
                                    .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.result_);
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
