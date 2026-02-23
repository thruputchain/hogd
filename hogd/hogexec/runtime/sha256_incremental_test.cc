#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "rpc_types.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class Sha256IncrementalTest : public ::testing::Test,
                              public NativeContractTestFramework {
public:
  Sha256IncrementalTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB, true)
  {}
};

TEST_F(Sha256IncrementalTest, BasicUpdateAndFinalize)
{
  NativeContractBuilder builder;
  constexpr static ContractAddress kAddress =
      NativeContractTestFramework::make_contract_address(0);
  constexpr static int32_t kContractId = 0;

  // Simple incremental hash in a single invocation frame.
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        const std::array<uint8_t, 4> first_chunk = {0, 1, 2, 3};
        const std::array<uint8_t, 4> second_chunk = {4, 5, 6, 7};

        auto [r1, _g1] =
            syscall_SHA256Update_inner(ctx.handler(), 0,
                                       std::as_bytes(std::span(first_chunk)),
                                       true);
        if (!r1.has_value() ||
            r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        auto [r2, _g2] =
            syscall_SHA256Update_inner(ctx.handler(), 0,
                                       std::as_bytes(std::span(second_chunk)),
                                       false);
        if (!r2.has_value() ||
            r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        SHA256Hash digest{};
        auto [r3, _g3] = syscall_SHA256Finalize_inner(ctx.handler(), 0, digest);
        if (!r3.has_value() ||
            r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        const auto expected_hash =
            sha256(std::vector<uint8_t>{0, 1, 2, 3, 4, 5, 6, 7});
        if (digest != expected_hash) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        return {};
      });

  auto native_contract = builder.build();
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kContractId, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr = make_native_contract(kContractId);
  write_contract(contract_xdr);
  write_deployment(kAddress, contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kAddress,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.result_);
}

TEST_F(Sha256IncrementalTest, FramesKeepIndependentHashBuffers)
{
  NativeContractBuilder builder;
  constexpr static ContractAddress kAddress =
      NativeContractTestFramework::make_contract_address(1);
  constexpr static int32_t kContractId = 1;

  // Parent updates buffer 0, invokes child that also uses buffer 0, then
  // continues hashing to confirm isolation between frames.
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        const std::array<uint8_t, 3> parent_prefix = {'p', 'a', 'r'};
        const std::array<uint8_t, 3> parent_suffix = {'e', 'n', 't'};

        auto [p1, _gp1] =
            syscall_SHA256Update_inner(ctx.handler(), 0,
                                       std::as_bytes(std::span(parent_prefix)),
                                       true);
        if (!p1.has_value() ||
            p1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        auto [invoke_status, _gas] =
            ctx.handler().invoke(kAddress, 1, UINT64_MAX, Calldata{},
                                 std::to_underlying(
                                     InvokeFlag::kInvokeFlagNone));
        if (!invoke_status.has_value() ||
            invoke_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        auto [p2, _gp2] =
            syscall_SHA256Update_inner(ctx.handler(), 0,
                                       std::as_bytes(std::span(parent_suffix)),
                                       false);
        if (!p2.has_value() ||
            p2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        SHA256Hash digest{};
        auto [p3, _gp3] =
            syscall_SHA256Finalize_inner(ctx.handler(), 0, digest);
        if (!p3.has_value() ||
            p3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        const auto expected_parent_hash =
            sha256(std::vector<uint8_t>{'p', 'a', 'r', 'e', 'n', 't'});
        if (digest != expected_parent_hash) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        return {};
      });

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      1, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        const std::array<uint8_t, 3> child_prefix = {'c', 'h', 'i'};
        const std::array<uint8_t, 3> child_suffix = {'l', 'd', '1'};

        auto [c1, _gc1] =
            syscall_SHA256Update_inner(ctx.handler(), 0,
                                       std::as_bytes(std::span(child_prefix)),
                                       true);
        if (!c1.has_value() ||
            c1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        auto [c2, _gc2] =
            syscall_SHA256Update_inner(ctx.handler(), 0,
                                       std::as_bytes(std::span(child_suffix)),
                                       false);
        if (!c2.has_value() ||
            c2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        SHA256Hash digest{};
        auto [c3, _gc3] =
            syscall_SHA256Finalize_inner(ctx.handler(), 0, digest);
        if (!c3.has_value() ||
            c3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        const auto expected_child_hash =
            sha256(std::vector<uint8_t>{'c', 'h', 'i', 'l', 'd', '1'});
        if (digest != expected_child_hash) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        return {};
      });

  auto native_contract = builder.build();
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kContractId, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr = make_native_contract(kContractId);
  write_contract(contract_xdr);
  write_deployment(kAddress, contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kAddress,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.result_);
}

} // namespace hogd
