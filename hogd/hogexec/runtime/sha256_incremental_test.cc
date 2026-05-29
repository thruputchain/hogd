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
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "syscall.hh"
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

        auto r1 = ctx.syscall<SyscallNo::kSHA256Update>(
            0, std::as_bytes(std::span(first_chunk)), true);
        if (!r1.has_value() ||
            r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto r2 = ctx.syscall<SyscallNo::kSHA256Update>(
            0, std::as_bytes(std::span(second_chunk)), false);
        if (!r2.has_value() ||
            r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        SHA256Hash digest{};
        auto r3 = ctx.syscall<SyscallNo::kSHA256Finalize>(0, digest);
        if (!r3.has_value() ||
            r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        const auto expected_hash =
            sha256(std::vector<uint8_t>{0, 1, 2, 3, 4, 5, 6, 7});
        if (digest != expected_hash) {
          return InvokeError{InvokeErrorCode::Deterministic};
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
  constexpr static ContractAddress kParentAddress =
      NativeContractTestFramework::make_contract_address(1);
  constexpr static ContractAddress kChildAddress =
      NativeContractTestFramework::make_contract_address(2);
  constexpr static int32_t kParentContractId = 1;
  constexpr static int32_t kChildContractId = 2;

  // Parent updates buffer 0, invokes a separate child contract that also uses
  // buffer 0, then continues hashing to confirm isolation between frames.
  NativeContractBuilder builder_parent;
  builder_parent.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        const std::array<uint8_t, 3> parent_prefix = {'p', 'a', 'r'};
        const std::array<uint8_t, 3> parent_suffix = {'e', 'n', 't'};

        auto p1 = ctx.syscall<SyscallNo::kSHA256Update>(
            0, std::as_bytes(std::span(parent_prefix)), true);
        if (!p1.has_value() ||
            p1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto invoke_status = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kChildAddress,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!invoke_status.has_value() ||
            invoke_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto p2 = ctx.syscall<SyscallNo::kSHA256Update>(
            0, std::as_bytes(std::span(parent_suffix)), false);
        if (!p2.has_value() ||
            p2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        SHA256Hash digest{};
        auto p3 = ctx.syscall<SyscallNo::kSHA256Finalize>(0, digest);
        if (!p3.has_value() ||
            p3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        const auto expected_parent_hash =
            sha256(std::vector<uint8_t>{'p', 'a', 'r', 'e', 'n', 't'});
        if (digest != expected_parent_hash) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  NativeContractBuilder builder_child;
  builder_child.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        const std::array<uint8_t, 3> child_prefix = {'c', 'h', 'i'};
        const std::array<uint8_t, 3> child_suffix = {'l', 'd', '1'};

        auto c1 = ctx.syscall<SyscallNo::kSHA256Update>(
            0, std::as_bytes(std::span(child_prefix)), true);
        if (!c1.has_value() ||
            c1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto c2 = ctx.syscall<SyscallNo::kSHA256Update>(
            0, std::as_bytes(std::span(child_suffix)), false);
        if (!c2.has_value() ||
            c2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        SHA256Hash digest{};
        auto c3 = ctx.syscall<SyscallNo::kSHA256Finalize>(0, digest);
        if (!c3.has_value() ||
            c3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        const auto expected_child_hash =
            sha256(std::vector<uint8_t>{'c', 'h', 'i', 'l', 'd', '1'});
        if (digest != expected_child_hash) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  auto native_contract_parent = builder_parent.build();
  auto native_contract_child = builder_child.build();
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kParentContractId, std::move(native_contract_parent));
  contract_map.emplace(kChildContractId, std::move(native_contract_child));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr_parent = make_native_contract(kParentContractId);
  write_contract(contract_xdr_parent);
  write_deployment(kParentAddress, contract_xdr_parent);

  auto contract_xdr_child = make_native_contract(kChildContractId);
  write_contract(contract_xdr_child);
  write_deployment(kChildAddress, contract_xdr_child);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kParentAddress,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.result_);
}

} // namespace hogd
