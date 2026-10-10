#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/base/network_config_params.h"
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

// An entrypoint that invokes `next` and requires the resulting child frame to
// report `expected`.
NativeContract::function_t
invoke_expecting(ContractAddress const &next,
                 InvocationFrameStatusCode expected)
{
  return [next, expected](
             NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
    CopyableInvokeOutputs outputs{};
    CopyableInvokeInputs inputs{
        .entrypoint_id = 0,
        .gas_limit = UINT64_MAX,
        .flags = std::to_underlying(InvokeFlag::None),
    };
    auto r =
        co_await ctx.syscall<SyscallNo::kInvoke>(next,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 outputs);
    if (!r.has_value() ||
        r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
      ADD_FAILURE() << "Invoke syscall failed outright";
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (outputs.frame_status != expected) {
      ADD_FAILURE() << "unexpected child frame status";
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    co_return InvokeStatus<void>{};
  };
}

// An entrypoint that invokes `next` forwarding `tokens` with `flags`, and
// requires the syscall itself to report `expected`.
//
// Every rejection pinned through this helper happens before a child frame is
// created, so each is a complete no-op: the outputs are seeded with values the
// syscall could not produce on that path -- nothing is written on a rejection,
// and an admitted call would write Return or Fault, never Abort -- so any
// write at all fails the test.
NativeContract::function_t
invoke_expecting_rejection(ContractAddress const &next, InvokeFlagSet flags,
                           std::vector<uint32_t> tokens,
                           SyscallErrorCode expected)
{
  return [next, flags, tokens = std::move(tokens), expected](
             NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
    CopyableInvokeOutputs outputs{
        .return_buffer_len = UINT32_MAX,
        .returned_auth_token_count = UINT32_MAX,
        .frame_status = InvocationFrameStatusCode::Abort,
    };
    CopyableInvokeInputs inputs{
        .entrypoint_id = 0,
        .gas_limit = UINT64_MAX,
        .flags = flags,
    };
    // The syscall takes a mutable span, so hand it one over a local copy.
    std::vector<uint32_t> forwarded = tokens;
    auto r =
        co_await ctx.syscall<SyscallNo::kInvoke>(next,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{forwarded},
                                                 inputs, outputs);
    if (!r.has_value() || r.value() != expected) {
      ADD_FAILURE() << "invoke reported the wrong syscall error code";
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (outputs.return_buffer_len != UINT32_MAX ||
        outputs.returned_auth_token_count != UINT32_MAX ||
        outputs.frame_status != InvocationFrameStatusCode::Abort) {
      ADD_FAILURE() << "a rejected invoke wrote to the caller's outputs";
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    co_return InvokeStatus<void>{};
  };
}

// The plain case: nothing wrong with the call except that the bound has no
// room for it.
NativeContract::function_t
invoke_expecting_depth_refusal(ContractAddress const &next)
{
  return invoke_expecting_rejection(
      next, std::to_underlying(InvokeFlag::None), {},
      SyscallErrorCode::kSyscallErrorCodeInvokeDepthExceeded);
}

// An entrypoint on a contract the bound must keep unreachable. Deployed and
// named as an invoke target so the refusal is observed at the call, but
// running its body is the failure these tests look for.
//
// It returns successfully so that entering it is caught by the caller's own
// assertion and not only by the ADD_FAILURE below: a body that faulted would
// leave the deepest frame of a chain reaching the root as a failure, which is
// also what a broken bound produces, whereas returning success makes the two
// outcomes differ.
NativeContract::function_t
unreachable_entrypoint()
{
  return [](NativeSyscallContext & /*ctx*/) -> x23::Async<InvokeStatus<void>> {
    ADD_FAILURE() << "a contract past the cross-invoke bound was entered";
    co_return InvokeStatus<void>{};
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

  // Deploys `deployed` contracts and runs `probe` as contract 0's entrypoint.
  // Contracts 1.. are deployed but must not be entered. The probe reports
  // through ADD_FAILURE and by failing its own frame, so a successful root is
  // the pass condition.
  void run_probe(int32_t deployed, NativeContract::function_t probe)
  {
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> probe_fns;
    probe_fns.emplace(0, std::move(probe));
    contract_map.emplace(0, std::make_unique<NativeContract>(
                                std::move(probe_fns)));
    for (int32_t i = 1; i < deployed; i++) {
      std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
      fns.emplace(0, unreachable_entrypoint());
      contract_map.emplace(i, std::make_unique<NativeContract>(std::move(fns)));
    }

    auto runtime_context =
        std::make_unique<NativeContext>(std::move(contract_map));
    auto handler = deploy_and_make_handler(deployed, *runtime_context);
    ASSERT_TRUE(handler.root_invoke()().result_);
  }

private:
  static NetworkConfig genesis_network_config(uint32_t max_cross_invoke_depth)
  {
    NetworkConfig config = stock_network_config();
    NetworkConfigParams::set_param(
        config, ConfigParamID::kConfigParamMaxCrossInvokeDepth,
        max_cross_invoke_depth);
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
      // whose invoke the bound refuses. The refusal is a syscall error the
      // caller can handle, so it keeps running and the transaction still
      // commits.
      const bool expect_refused =
          i == static_cast<int32_t>(kMaxCrossInvokeDepth) - 1;
      fns.emplace(0, expect_refused
                         ? invoke_expecting_depth_refusal(
                               make_contract_address(i + 1))
                         : invoke_expecting(make_contract_address(i + 1),
                                            InvocationFrameStatusCode::Return));
    }
    contract_map.emplace(i, std::make_unique<NativeContract>(std::move(fns)));
  }

  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));
  auto handler = deploy_and_make_handler(kChainLength, *runtime_context);

  auto res = handler.root_invoke()();
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

  auto res = handler.root_invoke()();

  // No frame is admitted, and there is no caller frame to receive the syscall
  // error, so the refusal surfaces as a deterministic failure of the whole
  // transaction. handle_root_host_fn_status maps every pre-frame rejection
  // that way, so this is unchanged by the refusal carrying its own code.
  ASSERT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeErrorCode::Deterministic);
  EXPECT_EQ(handler.get_most_recent_frame_output().status_code(),
            InvocationFrameStatusCode::Fault);
}

TEST_F(CrossInvokeDepthOneTest, RootInvocationRunsWithNoCrossInvokes)
{
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> caller_fns;
  caller_fns.emplace(0,
                     invoke_expecting_depth_refusal(make_contract_address(1)));
  contract_map.emplace(0,
                       std::make_unique<NativeContract>(std::move(caller_fns)));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> callee_fns;
  callee_fns.emplace(0, unreachable_entrypoint());
  contract_map.emplace(1,
                       std::make_unique<NativeContract>(std::move(callee_fns)));

  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));
  auto handler = deploy_and_make_handler(2, *runtime_context);

  auto res = handler.root_invoke()();

  // The root frame fits under the bound, and the refusal it sees is a syscall
  // error rather than a failed frame, so it runs to completion and the
  // transaction succeeds -- the callee simply never runs.
  ASSERT_TRUE(res.result_);
  EXPECT_EQ(handler.get_most_recent_frame_output().status_code(),
            InvocationFrameStatusCode::Return);
}

// The order in which invoke() rejects a call is observable: a contract sees
// exactly one error code, so which check wins is part of the syscall contract.
// These pin that order around the depth bound, so a refactor that reorders the
// checks fails here rather than silently changing what contracts observe.
//
// Every case runs under CrossInvokeDepthOneTest, where the root frame alone
// fills the bound, so each invoke below would be depth-refused if nothing else
// rejected it first. The one ordering not pinned here is the misaligned
// auth-token span: it is a check on the raw byte span, and the native syscall
// entry takes a typed span<uint32_t> that cannot express it.

// Reentrance and the depth bound are both conditions of the calling frame, and
// reentrance is checked first: it says the call can never succeed anywhere in
// this chain, where the bound says only that a shallower frame would admit it.
TEST_F(CrossInvokeDepthOneTest, ReentranceBeatsDepth)
{
  run_probe(1,
            invoke_expecting_rejection(
                make_contract_address(0), std::to_underlying(InvokeFlag::None),
                {}, SyscallErrorCode::kSyscallErrorCodeReentrance));
}

// The other side of that grouping: the depth bound outranks auth-token
// ownership, which is a question about the request rather than about the frame
// making it. The target is a different contract so reentrance does not win
// first.
TEST_F(CrossInvokeDepthOneTest, DepthBeatsUnownedAuthToken)
{
  run_probe(2, invoke_expecting_rejection(
                   make_contract_address(1),
                   std::to_underlying(InvokeFlag::None), {0xDEAD},
                   SyscallErrorCode::kSyscallErrorCodeInvokeDepthExceeded));
}

// Duplicate ids are rejected by syscall_Invoke_inner before the handler is
// reached at all.
TEST_F(CrossInvokeDepthOneTest, DuplicateAuthTokenIdsBeatDepth)
{
  run_probe(2, invoke_expecting_rejection(
                   make_contract_address(1),
                   std::to_underlying(InvokeFlag::None), {7, 7},
                   SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput));
}

// validate_invoke_flags admits only TryInvoke and ReadOnly, and runs before
// the handler is reached. 0x8 is not a defined InvokeFlag.
TEST_F(CrossInvokeDepthOneTest, InvalidFlagsBeatDepth)
{
  run_probe(2, invoke_expecting_rejection(
                   make_contract_address(1), 0x8, {},
                   SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput));
}

// The load-bearing direction: resolve_runtime sits *after* the depth guard, so
// a target that is not deployed still reports the bound. Hoisting deployment
// resolution above the guard would silently change this to
// kInvalidSyscallInput.
TEST_F(CrossInvokeDepthOneTest, DepthBeatsUnresolvableTarget)
{
  run_probe(1, invoke_expecting_depth_refusal(make_contract_address(9)));
}

// Control for DepthBeatsUnresolvableTarget. This fixture leaves room under the
// bound, so the same invoke of an undeployed address gets past the depth guard
// and reports the missing deployment instead. Without it that test would still
// pass if the guard swallowed every call, and the ordering it claims to pin
// would not be pinned at all.
TEST_F(CrossInvokeDepthTest, UnresolvableTargetReportedWhenDepthAllows)
{
  run_probe(1,
            invoke_expecting_rejection(
                make_contract_address(9), std::to_underlying(InvokeFlag::None),
                {}, SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput));
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
