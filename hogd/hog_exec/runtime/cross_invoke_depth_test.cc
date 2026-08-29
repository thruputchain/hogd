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

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

namespace {

constexpr uint32_t kMaxCrossInvokeDepth = 3;
// One contract longer than the limit admits, so the last one is deployed and
// invoked but must never be entered.
constexpr int32_t kChainLength = kMaxCrossInvokeDepth + 1;

// An entrypoint that invokes `next` and requires the resulting child frame to
// report `expected`.
NativeContract::function_t
invoke_expecting(ContractAddress const &next,
                 InvocationFrameStatusCode expected)
{
  return [next,
          expected](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    CopyableInvokeOutputs outputs{};
    CopyableInvokeInputs inputs{
        .entrypoint_id = 0,
        .gas_limit = UINT64_MAX,
        .flags = std::to_underlying(InvokeFlag::None),
    };
    auto r =
        ctx.syscall<SyscallNo::kInvoke>(next, std::span<const std::byte>{},
                                        std::span<uint32_t>{}, inputs, outputs);
    if (!r.has_value() ||
        r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
      ADD_FAILURE() << "Invoke syscall failed outright";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (outputs.frame_status != expected) {
      ADD_FAILURE() << "unexpected child frame status";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
  };
}

// An entrypoint on a contract the bound must keep unreachable. Deployed and
// named as an invoke target so the refusal is observed at the call, but
// running its body is the failure these tests look for.
//
// It returns successfully so that running it is distinguishable from being
// refused: a refused invoke reports Fault, and so would a body that failed, so
// a body that faulted would leave the caller's status assertion passing for the
// wrong reason. Returning success makes the caller observe Return instead, and
// the deepest frame of a chain reach the root as a success -- either way the
// assertion fails on its own, without relying on the ADD_FAILURE below.
NativeContract::function_t
unreachable_entrypoint()
{
  return [](NativeSyscallContext & /*ctx*/) noexcept -> InvokeStatus<void> {
    ADD_FAILURE() << "a contract past the cross-invoke bound was entered";
    return {};
  };
}

} // namespace

// kConfigParamMaxCrossInvokeDepth bounds how many invocation frames one
// transaction may hold open. It is read off the DbTip, so these cover both the
// enforcement and the parameter reaching the executor as genesis set it.
class CrossInvokeDepthTestBase : public ::testing::Test,
                                 public NativeContractTestFramework {
protected:
  explicit CrossInvokeDepthTestBase(uint32_t max_cross_invoke_depth)
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb, true, std::nullopt,
          genesis_network_config(max_cross_invoke_depth))
  {}

  // Deploys `count` native contracts at make_contract_address(0..count-1) and
  // returns a handler whose root invocation enters contract 0's entrypoint 0.
  SystemCallHandler deploy_and_make_handler(int32_t count,
                                            NativeContext &runtime_context)
  {
    for (int32_t i = 0; i < count; i++) {
      auto contract_obj = make_native_contract(i);
      write_contract(contract_obj);
      write_deployment(make_contract_address(i), contract_obj);
    }

    return make_new_system_call_handler(
        Transaction{.invocation =
                        Invocation{.invoked_address = make_contract_address(0),
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                    .uniquifier = make_new_uniquifier()},
        runtime_context);
  }

private:
  static NetworkConfig genesis_network_config(uint32_t max_cross_invoke_depth)
  {
    NetworkConfig config = stock_network_config();
    config.max_cross_invoke_depth = max_cross_invoke_depth;
    return config;
  }
};

class CrossInvokeDepthTest : public CrossInvokeDepthTestBase {
public:
  CrossInvokeDepthTest() : CrossInvokeDepthTestBase(kMaxCrossInvokeDepth) {}
};

// Zero is the low edge of the bound: the root invocation is charged against it
// like any other frame, so nothing runs at all.
class CrossInvokeDepthZeroTest : public CrossInvokeDepthTestBase {
public:
  CrossInvokeDepthZeroTest() : CrossInvokeDepthTestBase(0) {}
};

// One is the first value that runs a transaction: the root frame fits, and
// nothing it calls does.
class CrossInvokeDepthOneTest : public CrossInvokeDepthTestBase {
public:
  CrossInvokeDepthOneTest() : CrossInvokeDepthTestBase(1) {}
};

TEST_F(CrossInvokeDepthTest, GenesisValueBoundsOpenFrames)
{
  // A chain of distinct contracts rather than one self-call: reentrance would
  // reject a self-invoke before the depth check is reached.
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  for (int32_t i = 0; i < kChainLength; i++) {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    if (i == kChainLength - 1) {
      fns.emplace(0, unreachable_entrypoint());
    }
    else {
      // Contract i runs with i + 1 frames open (the root frame plus one per
      // contract entered), so contract kMaxCrossInvokeDepth - 1 is the first
      // whose invoke would exceed the limit. Exceeding it is reported as a
      // faulted child frame, so the caller keeps running and the transaction
      // still commits.
      const bool expect_refused =
          i == static_cast<int32_t>(kMaxCrossInvokeDepth) - 1;
      fns.emplace(0, invoke_expecting(make_contract_address(i + 1),
                                      expect_refused
                                          ? InvocationFrameStatusCode::Fault
                                          : InvocationFrameStatusCode::Return));
    }
    contract_map.emplace(i, std::make_unique<NativeContract>(std::move(fns)));
  }

  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));
  auto handler = deploy_and_make_handler(kChainLength, *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.result_);
}

TEST_F(CrossInvokeDepthZeroTest, RootInvocationIsRefused)
{
  // Deployed, callable, and reached by a well-formed transaction: only the
  // bound stops it.
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, unreachable_entrypoint());
  contract_map.emplace(0, std::make_unique<NativeContract>(std::move(fns)));

  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));
  auto handler = deploy_and_make_handler(1, *runtime_context);

  auto res = handler.root_invoke();

  // No frame is admitted, so there is no caller left to absorb the refusal as
  // a faulted child: it surfaces as a deterministic failure of the whole
  // transaction.
  ASSERT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeErrorCode::Deterministic);
  EXPECT_EQ(handler.get_most_recent_frame_output().status_code(),
            InvocationFrameStatusCode::Fault);
}

TEST_F(CrossInvokeDepthOneTest, RootInvocationRunsWithNoCrossInvokes)
{
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> caller_fns;
  caller_fns.emplace(0, invoke_expecting(make_contract_address(1),
                                         InvocationFrameStatusCode::Fault));
  contract_map.emplace(0,
                       std::make_unique<NativeContract>(std::move(caller_fns)));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> callee_fns;
  callee_fns.emplace(0, unreachable_entrypoint());
  contract_map.emplace(1,
                       std::make_unique<NativeContract>(std::move(callee_fns)));

  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));
  auto handler = deploy_and_make_handler(2, *runtime_context);

  auto res = handler.root_invoke();

  // The root frame fits under the bound and the refusal it sees is a faulted
  // child frame, so it runs to completion and the transaction succeeds -- the
  // callee simply never runs.
  ASSERT_TRUE(res.result_);
  EXPECT_EQ(handler.get_most_recent_frame_output().status_code(),
            InvocationFrameStatusCode::Return);
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
