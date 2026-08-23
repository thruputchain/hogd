#include <gtest/gtest.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_entry_outer.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/format.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class DeleteRecreateTest : public ::testing::TestWithParam<TestDB>,
                           public NativeContractTestFramework {
public:
  DeleteRecreateTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override {}
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_P(DeleteRecreateTest, DeleteRecreateBytestring)
{
  // Storage contract: 0 = read-empty then write bytestring; 1 = delete key.
  // Orchestrator: invokes storage 0, then 1, then 0 again (no self-reentrance).
  // Expected: all three nested invokes succeed.
  static constexpr ContractAddress kAddressStorage = make_contract_address(0);
  static constexpr ContractAddress kAddressOrchestrator =
      make_contract_address(1);

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_storage;
    fns_storage.emplace(
        0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto key = make_storage_key("test");
          // Read the storage key, expecting it to be empty
          std::vector<std::byte> buf(1024);
          RuntimeMemoryLength len = 0;
          auto [status, _g1] = syscall_BytestringGet_inner(
              ctx.handler(), key, std::as_writable_bytes(std::span(buf)),
              len)();
          if (!status.has_value()) {
            ADD_FAILURE() << "Failed to get bytestring: " << status.error();
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (status.value() !=
              SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist) {
            ADD_FAILURE() << "Expected storage key to be empty, but got: "
                          << x23::show(status.value());
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          // Write a bytestring to the storage key
          Bytestring bytestring = make_bytestring("hello");
          auto [write_status, _g2] = syscall_BytestringSet_inner(
              ctx.handler(), key, std::as_bytes(std::span(bytestring)),
              std::to_underlying(StorageDeltaFlags::None))();
          if (!write_status.has_value()) {
            ADD_FAILURE() << "Failed to write bytestring: "
                          << write_status.error();
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (write_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Expected write to succeed, but got: "
                          << x23::show(write_status.value());
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          co_return InvokeStatus<void>{};
        });
    fns_storage.emplace(
        1, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto key = make_storage_key("test");
          // Delete the storage key
          auto delete_status =
              co_await ctx.syscall<SyscallNo::kDeleteObject>(key, 0);
          if (!delete_status.has_value()) {
            ADD_FAILURE() << "Failed to delete object: "
                          << delete_status.error();
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (delete_status.value() !=
              SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Expected delete to succeed, but got: "
                          << x23::show(delete_status.value());
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          co_return InvokeStatus<void>{};
        });
    auto native_contract_storage =
        std::make_unique<NativeContract>(std::move(fns_storage));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_orchestrator;
    fns_orchestrator.emplace(
        0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto r0 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = static_cast<GasAmount>(UINT64_MAX),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            CopyableInvokeOutputs outputs{};
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kAddressStorage, ctx.handler().get_calldata(),
                std::span<uint32_t>{}, inputs, outputs);
          }();

          if (!r0.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto r1 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 1,
                .gas_limit = static_cast<GasAmount>(UINT64_MAX),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            CopyableInvokeOutputs outputs{};
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kAddressStorage, std::span<const std::byte>{},
                std::span<uint32_t>{}, inputs, outputs);
          }();

          if (!r1.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto r2 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = static_cast<GasAmount>(UINT64_MAX),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            CopyableInvokeOutputs outputs{};
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kAddressStorage, std::span<const std::byte>{},
                std::span<uint32_t>{}, inputs, outputs);
          }();

          if (!r2.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });
    auto native_contract_orchestrator =
        std::make_unique<NativeContract>(std::move(fns_orchestrator));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_storage));
    contract_map.emplace(1, std::move(native_contract_orchestrator));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {kAddressStorage, kAddressOrchestrator};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  // now invoke the contract

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = kAddressOrchestrator,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler = make_new_system_call_handler(handler_tx, *context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction()();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  // Verify that the bytestring was written (from the second call to method 0)
  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(kAddressStorage, "test"));
  ASSERT_TRUE(read_res.has_value());
  auto expect = make_bytestring("hello");
  ASSERT_EQ(read_res.value().bytestring(), expect);
}

TEST_P(DeleteRecreateTest, DeleteRecreateNNInt)
{
  // Storage contract: 0 = read-empty then write nnint; 1 = delete key.
  // Orchestrator: invokes storage 0, then 1, then 0 again (no self-reentrance).
  // Expected: all three nested invokes succeed.
  static constexpr ContractAddress kAddressStorage = make_contract_address(0);
  static constexpr ContractAddress kAddressOrchestrator =
      make_contract_address(1);

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_storage;
    fns_storage.emplace(
        0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto key = make_storage_key("test");
          // Read the storage key, expecting it to be empty
          int128_t current_set = 0;
          bool current_set_opt = false;
          int128_t current_add = 0;
          auto status = co_await ctx.syscall<SyscallNo::kNonnegativeIntGet>(
              key, true, current_set, current_set_opt, current_add);
          if (!status.has_value()) {
            ADD_FAILURE() << "Failed to get nnint: " << status.error();
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (status.value() !=
              SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist) {
            ADD_FAILURE() << "Expected storage key to be empty, but got: "
                          << x23::show(status.value());
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          // Write a nonnegative integer to the storage key
          int128_t set_value = 42;
          auto [write_status, _g2] = syscall_NonnegativeIntSetAdd_inner(
              ctx.handler(), key, &set_value, 0,
              std::to_underlying(StorageDeltaFlags::None))();
          if (!write_status.has_value()) {
            ADD_FAILURE() << "Failed to write nnint: " << write_status.error();
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (write_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Expected write to succeed, but got: "
                          << x23::show(write_status.value());
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          co_return InvokeStatus<void>{};
        });
    fns_storage.emplace(
        1, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto key = make_storage_key("test");
          // Delete the storage key
          auto delete_status =
              co_await ctx.syscall<SyscallNo::kDeleteObject>(key, 0);
          if (!delete_status.has_value()) {
            ADD_FAILURE() << "Failed to delete object: "
                          << delete_status.error();
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (delete_status.value() !=
              SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Expected delete to succeed, but got: "
                          << x23::show(delete_status.value());
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          co_return InvokeStatus<void>{};
        });
    auto native_contract_storage =
        std::make_unique<NativeContract>(std::move(fns_storage));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_orchestrator;
    fns_orchestrator.emplace(
        0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto r0 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = static_cast<GasAmount>(UINT64_MAX),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            CopyableInvokeOutputs outputs{};
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kAddressStorage, ctx.handler().get_calldata(),
                std::span<uint32_t>{}, inputs, outputs);
          }();

          if (!r0.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto r1 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 1,
                .gas_limit = static_cast<GasAmount>(UINT64_MAX),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            CopyableInvokeOutputs outputs{};
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kAddressStorage, std::span<const std::byte>{},
                std::span<uint32_t>{}, inputs, outputs);
          }();

          if (!r1.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto r2 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = static_cast<GasAmount>(UINT64_MAX),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            CopyableInvokeOutputs outputs{};
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kAddressStorage, std::span<const std::byte>{},
                std::span<uint32_t>{}, inputs, outputs);
          }();

          if (!r2.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });
    auto native_contract_orchestrator =
        std::make_unique<NativeContract>(std::move(fns_orchestrator));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_storage));
    contract_map.emplace(1, std::move(native_contract_orchestrator));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {kAddressStorage, kAddressOrchestrator};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  // now invoke the contract

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = kAddressOrchestrator,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler = make_new_system_call_handler(handler_tx, *context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction()();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  // Verify that the nnint was written (from the second call to method 0)
  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(kAddressStorage, "test"));
  ASSERT_TRUE(read_res.has_value());
  ASSERT_EQ(read_res.value().type(), StorageItemType::kItemTypeNonnegativeInt);
  auto nnint_item = read_res.value().nnint();
  ASSERT_EQ(from_xdr(nnint_item), 42);
}

TEST_P(DeleteRecreateTest, DeleteRecreateRetrocausalQueue)
{
  // Storage contract: 0 = consume-empty then insert; 1 = delete key.
  // Orchestrator: invokes storage 0, then 1, then 0 again (no self-reentrance).
  // Expected: all three nested invokes succeed.
  static constexpr ContractAddress kAddressStorage = make_contract_address(0);
  static constexpr ContractAddress kAddressOrchestrator =
      make_contract_address(1);

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_storage;
    fns_storage.emplace(
        0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto key = make_storage_key("test");
          // Consume from the storage key, expecting it to be empty
          std::vector<std::byte> buf(1024);
          RuntimeMemoryLength written_len = 0;
          auto [status, _g1] = syscall_RetrocausalQueueConsume_inner(
              ctx.handler(), key, false, std::as_writable_bytes(std::span(buf)),
              written_len)();
          if (!status.has_value()) {
            ADD_FAILURE() << "Failed to consume from queue: " << status.error();
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (status.value() !=
              SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry) {
            ADD_FAILURE() << "Expected queue to be empty, but got: "
                          << x23::show(status.value());
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          // Insert an item into the queue
          QueueEntry entry = make_bytestring("item");
          auto [insert_status, _g2] = syscall_RetrocausalQueueInsert_inner(
              ctx.handler(), key, std::as_bytes(std::span(entry)))();
          if (!insert_status.has_value()) {
            ADD_FAILURE() << "Failed to insert into queue: "
                          << insert_status.error();
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (insert_status.value() !=
              SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Expected insert to succeed, but got: "
                          << x23::show(insert_status.value());
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          co_return InvokeStatus<void>{};
        });
    fns_storage.emplace(
        1, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto key = make_storage_key("test");
          // Delete the storage key
          auto delete_status =
              co_await ctx.syscall<SyscallNo::kDeleteObject>(key, 0);
          if (!delete_status.has_value()) {
            ADD_FAILURE() << "Failed to delete object: "
                          << delete_status.error();
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (delete_status.value() !=
              SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Expected delete to succeed, but got: "
                          << x23::show(delete_status.value());
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          co_return InvokeStatus<void>{};
        });
    auto native_contract_storage =
        std::make_unique<NativeContract>(std::move(fns_storage));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_orchestrator;
    fns_orchestrator.emplace(
        0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto r0 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = static_cast<GasAmount>(UINT64_MAX),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            CopyableInvokeOutputs outputs{};
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kAddressStorage, ctx.handler().get_calldata(),
                std::span<uint32_t>{}, inputs, outputs);
          }();

          if (!r0.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto r1 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 1,
                .gas_limit = static_cast<GasAmount>(UINT64_MAX),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            CopyableInvokeOutputs outputs{};
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kAddressStorage, std::span<const std::byte>{},
                std::span<uint32_t>{}, inputs, outputs);
          }();

          if (!r1.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto r2 =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = static_cast<GasAmount>(UINT64_MAX),
                .flags = std::to_underlying(InvokeFlag::None),
            };
            CopyableInvokeOutputs outputs{};
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                kAddressStorage, std::span<const std::byte>{},
                std::span<uint32_t>{}, inputs, outputs);
          }();

          if (!r2.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });
    auto native_contract_orchestrator =
        std::make_unique<NativeContract>(std::move(fns_orchestrator));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_storage));
    contract_map.emplace(1, std::move(native_contract_orchestrator));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {kAddressStorage, kAddressOrchestrator};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  // now invoke the contract

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = kAddressOrchestrator,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler = make_new_system_call_handler(handler_tx, *context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction()();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  // Verify that the queue exists and has exactly one item (from the second call
  // to method 0)
  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(kAddressStorage, "test"));
  ASSERT_TRUE(read_res.has_value());
  ASSERT_EQ(read_res.value().type(),
            StorageItemType::kItemTypeRetrocausalQueue);

  // Verify there's exactly one item by consuming: first consume should succeed,
  // second should fail
  auto make_verify_context = [&]() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_verify;
    fns_verify.emplace(
        0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto key = make_storage_key("test");
          std::vector<std::byte> buf(1024);
          RuntimeMemoryLength written_len = 0;
          // First consume should succeed
          auto [status1, _g1] = syscall_RetrocausalQueueConsume_inner(
              ctx.handler(), key, false, std::as_writable_bytes(std::span(buf)),
              written_len)();
          if (!status1.has_value() ||
              status1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          // Second consume should fail (queue empty)
          auto [status2, _g2] = syscall_RetrocausalQueueConsume_inner(
              ctx.handler(), key, false, std::as_writable_bytes(std::span(buf)),
              written_len)();
          if (!status2.has_value() ||
              status2.value() !=
                  SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });
    auto native_contract_0 =
        std::make_unique<NativeContract>(std::move(fns_verify));
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_0));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto verify_context = make_verify_context();

  auto verify_handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = kAddressStorage,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto verify_handler =
      make_new_system_call_handler(verify_handler_tx, *verify_context);

  auto verify_res = verify_handler.root_invoke()();
  ASSERT_TRUE(verify_res.is_root_success());
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, DeleteRecreateTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
