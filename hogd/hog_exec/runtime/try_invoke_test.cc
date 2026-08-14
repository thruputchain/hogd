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

#include "built_contracts/itc_erc20.h"
#include "db_conflict.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hog_exec_constants.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
#include "hogd/hog_exec/precompiles/precompile_native_token.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/serialize_calldata.h"
#include "hogd/hog_exec/runtime/syscall_entry_outer.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "model_checker/async.h"
#include "model_checker/work_queue.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class TryInvokeTest : public ::testing::TestWithParam<TestDB>,
                      public NativeContractTestFramework {
public:
  TryInvokeTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override {}

  static std::tuple<uint64_t, uint64_t> make_amount(uint128_t amount)
  {
    return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
  }
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// Not really a try invoke test, but it's a convenient way to test general
// invocation.
TEST_P(TryInvokeTest, ReadWriteBytestring)
{ // Storage contract: 0 writes calldata to a hardcoded key; 1 reads it back
  // into the return buffer.  Orchestrator contract: invokes storage 0 then
  // storage 1 and checks the returned bytes match root calldata.
  ContractAddress addr_storage = make_contract_address(0);
  ContractAddress addr_orchestrator = make_contract_address(1);

  auto make_context = [addr_storage]() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_storage;
    fns_storage.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          auto const &calldata = ctx.handler().get_calldata();
          auto key = make_storage_key("test");
          auto [res, _gas] = syscall_BytestringSet_inner(
              ctx.handler(), key, std::as_bytes(std::span(calldata)),
              std::to_underlying(StorageDeltaFlags::None));
          if (!res.has_value() ||
              res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE()
                << "ReadWriteBytestring: Failed to set bytestring in method 0";
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          return {};
        });
    fns_storage.emplace(
        1, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          auto key = make_storage_key("test");
          auto res = ctx.handler().get_bytestring(key);
          if (!res.has_value()) {
            return res.error().to_invoke_error();
          }
          if (!res.value().has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          ReturnBuffer out;
          out.resize(res.value().value()->size());
          if (!out.empty()) {
            std::memcpy(out.data(), res.value().value()->data(),
                        res.value().value()->size());
          }

          ctx.handler().set_return_buffer(std::move(out), false);
          return {};
        });
    auto native_contract_storage =
        std::make_unique<NativeContract>(std::move(fns_storage));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_orch;
    fns_orch.emplace(
        0,
        [addr_storage](
            NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          auto calldata = ctx.handler().get_calldata();

          auto r0 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::None),
            };
            return ctx.syscall<SyscallNo::kInvoke>(addr_storage,
                                                   ctx.handler().get_calldata(),
                                                   std::span<uint32_t>{},
                                                   inputs,
                                                   CopyableInvokeOutputs{});
          }();

          if (!r0.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto r1 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 1,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::None),
            };
            return ctx.syscall<SyscallNo::kInvoke>(addr_storage,
                                                   std::span<const std::byte>{},
                                                   std::span<uint32_t>{},
                                                   inputs,
                                                   CopyableInvokeOutputs{});
          }();

          if (!r1.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto returned_buffer = ctx.handler().get_return_buffer();
          if (!returned_buffer.has_value() ||
              !returned_buffer.value().has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto const &returned_buffer_data = returned_buffer.value().value();

          if (returned_buffer_data->size() != calldata.size()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          if (!returned_buffer_data->empty()) {
            if (std::memcmp(returned_buffer_data->data(), calldata.data(),
                            calldata.size()) != 0) {
              return InvokeError{InvokeErrorCode::Deterministic};
            }
          }

          return {};
        });
    auto native_contract_orchestrator =
        std::make_unique<NativeContract>(std::move(fns_orch));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_storage));
    contract_map.emplace(1, std::move(native_contract_orchestrator));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {addr_storage, addr_orchestrator};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  // now invoke the contract

  SignedTransaction signed_tx{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = addr_orchestrator,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {0, 1, 2, 3, 4, 5}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler = make_new_system_call_handler(signed_tx, *context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction();
  ASSERT_TRUE(r == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = signed_tx})());

  advance_block();

  // Now runs the same transaction again, with an empty string for the
  // bytestring/calldata.
  SignedTransaction signed_tx2{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = addr_orchestrator,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler2 = make_new_system_call_handler(signed_tx2, *context);
  auto res2 = handler2.root_invoke();
  ASSERT_TRUE(res2.is_root_success());

  auto r2 = handler2.try_commit_transaction();
  ASSERT_TRUE(r2 == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler2.emit_events(ExecResult::ExecResultOK,
                           ProposedTransaction{.signed_tx = signed_tx2})());
}

TEST_P(TryInvokeTest, TryInvokeGoodWorkflow)
{ // Writer: method 0 writes "hello" to a bytestring key. Caller: method 0
  // try_invokes writer:0 (no self-reentrance).
  static constexpr ContractAddress kAddressWriter = make_contract_address(0);
  static constexpr ContractAddress kAddressCaller = make_contract_address(1);

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_writer;
    fns_writer.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          auto key = make_storage_key("test");
          auto [res, _gas] = syscall_BytestringSet_inner(
              ctx.handler(), key, std::as_bytes(std::span{"hello"}),
              std::to_underlying(StorageDeltaFlags::None));
          if (!res.has_value() ||
              res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "TryInvokeGoodWorkflow: Failed to set "
                             "bytestring on writer";
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          return {};
        });
    auto native_contract_writer =
        std::make_unique<NativeContract>(std::move(fns_writer));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_caller;
    fns_caller.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          auto r0 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter,
                                                   ctx.handler().get_calldata(),
                                                   std::span<uint32_t>{},
                                                   inputs,
                                                   CopyableInvokeOutputs{});
          }();

          if (!r0.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          if (r0.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          return {};
        });
    auto native_contract_caller =
        std::make_unique<NativeContract>(std::move(fns_caller));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_writer));
    contract_map.emplace(1, std::move(native_contract_caller));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {kAddressWriter, kAddressCaller};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  // now invoke the contract

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressCaller,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction();
  ASSERT_TRUE(r == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
  advance_block();

  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(kAddressWriter, "test"));
  ASSERT_TRUE(read_res.has_value());
  auto expect_span = std::as_bytes(std::span{"hello"});
  Bytestring expect;
  expect.resize(expect_span.size());
  std::memcpy(expect.data(), expect_span.data(), expect_span.size());
  ASSERT_EQ(read_res.value().bytestring(), expect);
}

namespace {

constexpr ContractAddress kTryInvokeHandleErrorsWriter =
    NativeContractTestFramework::make_contract_address(0);
constexpr ContractAddress kTryInvokeHandleErrorsFault =
    NativeContractTestFramework::make_contract_address(4);

// Shared native contracts for TryInvokeHandleErrors_* tests: writer,
// orchestrator, fault, and bridge (see comments on each TEST_P).
std::unique_ptr<RuntimeContext>
make_try_invoke_handle_errors_context()
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_w;
  fns_w.emplace(0,
                [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto key =
                      NativeContractTestFramework::make_storage_key("test");
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key, std::as_bytes(std::span{"hello"}),
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE() << "TryInvokeHandleErrors: Failed to set "
                                     "bytestring in method 0";
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  return {};
                });
  fns_w.emplace(1,
                [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto key =
                      NativeContractTestFramework::make_storage_key("test");
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key, std::as_bytes(std::span{"world"}),
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE() << "TryInvokeHandleErrors: Failed to set "
                                     "bytestring in method 1";
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  return {};
                });
  auto native_contract_writer =
      std::make_unique<NativeContract>(std::move(fns_w));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_o;
  fns_o.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto r0 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kTryInvokeHandleErrorsWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r0.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        auto r1 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 1,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kTryInvokeHandleErrorsWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r1.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  fns_o.emplace(
      1, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto r0 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kTryInvokeHandleErrorsWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r0.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return InvokeError{InvokeErrorCode::Deterministic};
      });
  auto native_contract_orchestrator =
      std::make_unique<NativeContract>(std::move(fns_o));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_f;
  fns_f.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto r0 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kTryInvokeHandleErrorsWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r0.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return InvokeError{InvokeErrorCode::Deterministic};
      });
  auto native_contract_fault =
      std::make_unique<NativeContract>(std::move(fns_f));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_b;
  fns_b.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto r0 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kTryInvokeHandleErrorsFault,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r0.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        auto r1 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 1,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kTryInvokeHandleErrorsWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r1.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto native_contract_bridge =
      std::make_unique<NativeContract>(std::move(fns_b));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_writer));
  contract_map.emplace(1, std::move(native_contract_orchestrator));
  contract_map.emplace(2, std::move(native_contract_fault));
  contract_map.emplace(3, std::move(native_contract_bridge));
  return std::make_unique<NativeContext>(std::move(contract_map));
}

} // namespace

// Writer: 0 = "hello", 1 = "world" on shared key.
// Orchestrator: 0 = try_invoke writer 0 then try_invoke writer 1; 1 =
// try_invoke writer 0 then error.
TEST_P(TryInvokeTest, TryInvokeHandleErrorsSequentialGoodTryInvokes)
{
  auto context = make_try_invoke_handle_errors_context();

  auto writer_xdr = make_native_contract(0);
  auto orchestrator_xdr = make_native_contract(1);
  auto fault_xdr = make_native_contract(2);
  auto bridge_xdr = make_native_contract(3);
  write_contract(writer_xdr);
  write_contract(orchestrator_xdr);
  write_contract(fault_xdr);
  write_contract(bridge_xdr);
  write_deployment(kTryInvokeHandleErrorsWriter, writer_xdr);
  setup_grant_storage_token(kTryInvokeHandleErrorsWriter, 1'000'000'000);
  write_deployment(kTryInvokeHandleErrorsFault, fault_xdr);
  setup_grant_storage_token(kTryInvokeHandleErrorsFault, 1'000'000'000);

  // Sequential good try invokes: orchestrator at address 1, method 0.
  write_deployment(make_contract_address(1), orchestrator_xdr);
  setup_grant_storage_token(make_contract_address(1), 1'000'000'000);

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = make_contract_address(1),
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
  advance_block();
  // Check that the last write was "world"
  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(kTryInvokeHandleErrorsWriter, "test"));
  ASSERT_TRUE(read_res.has_value());
  auto expect_span = std::as_bytes(std::span{"world"});
  Bytestring expect;
  expect.resize(expect_span.size());
  std::memcpy(expect.data(), expect_span.data(), expect_span.size());
  ASSERT_EQ(read_res.value().bytestring(), expect);
}

TEST_P(TryInvokeTest, TryInvokeHandleErrorsUnwindFrameWithTryInvoke)
{
  auto context = make_try_invoke_handle_errors_context();

  auto writer_xdr = make_native_contract(0);
  auto orchestrator_xdr = make_native_contract(1);
  auto fault_xdr = make_native_contract(2);
  auto bridge_xdr = make_native_contract(3);
  write_contract(writer_xdr);
  write_contract(orchestrator_xdr);
  write_contract(fault_xdr);
  write_contract(bridge_xdr);
  write_deployment(kTryInvokeHandleErrorsWriter, writer_xdr);
  setup_grant_storage_token(kTryInvokeHandleErrorsWriter, 1'000'000'000);
  write_deployment(kTryInvokeHandleErrorsFault, fault_xdr);
  setup_grant_storage_token(kTryInvokeHandleErrorsFault, 1'000'000'000);

  // Unwinding a frame that contains a try_invoke: orchestrator at address 2,
  // method 1.
  write_deployment(make_contract_address(2), orchestrator_xdr);
  setup_grant_storage_token(make_contract_address(2), 1'000'000'000);

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = make_contract_address(2),
                                   .entrypoint = entrypoint_from_method_id(1),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *context);
  auto res = handler.root_invoke();
  ASSERT_FALSE(res.result_); // Should return error
  auto commit_res = handler.try_commit_transaction();
  // transaction should not commit
  ASSERT_TRUE(commit_res == ExecResult::ExecResultTxDeterministicError);
  advance_block();
  // Storage should not be updated
  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(kTryInvokeHandleErrorsWriter, "test"));
  // Should be empty or not present
  ASSERT_FALSE(read_res.has_value());
}

// Fault: 0 = same as orchestrator 1 (try_invoke writer 0, deterministic error)
// — callee of a regular invoke from the bridge.
// Bridge: 0 = regular invoke fault:0, then try_invoke writer 1.
TEST_P(TryInvokeTest, TryInvokeHandleErrorsBridgeInvokeFaultThenTryInvokeWriter)
{
  auto context = make_try_invoke_handle_errors_context();

  auto writer_xdr = make_native_contract(0);
  auto orchestrator_xdr = make_native_contract(1);
  auto fault_xdr = make_native_contract(2);
  auto bridge_xdr = make_native_contract(3);
  write_contract(writer_xdr);
  write_contract(orchestrator_xdr);
  write_contract(fault_xdr);
  write_contract(bridge_xdr);
  write_deployment(kTryInvokeHandleErrorsWriter, writer_xdr);
  setup_grant_storage_token(kTryInvokeHandleErrorsWriter, 1'000'000'000);
  write_deployment(kTryInvokeHandleErrorsFault, fault_xdr);
  setup_grant_storage_token(kTryInvokeHandleErrorsFault, 1'000'000'000);

  // Bridge at address 3, method 0: regular invoke fault, then try_invoke
  // writer.
  write_deployment(make_contract_address(3), bridge_xdr);
  setup_grant_storage_token(make_contract_address(3), 1'000'000'000);

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = make_contract_address(3),
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
  advance_block();
  // Storage should be updated to "world"
  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(kTryInvokeHandleErrorsWriter, "test"));
  ASSERT_TRUE(read_res.has_value());
  auto expect_span = std::as_bytes(std::span{"world"});
  Bytestring expect;
  expect.resize(expect_span.size());
  std::memcpy(expect.data(), expect_span.data(), expect_span.size());
  ASSERT_EQ(read_res.value().bytestring(), expect);
}

TEST_P(TryInvokeTest, TryInvokeNNIntAdd)
{
  // Test on try invoke with nnint_add.
  //
  // Callee contract — method 0: loads the calldata, interprets it as one int8_t
  // value [a], and performs an nnint_add of (a) on a storage key.
  //
  // Caller contract — method 0: invokes callee method 0 in a try_invoke frame,
  // with arguments [-5]. Check that that invocation failed, and then return a
  // deterministic error.
  //
  // The test invokes the caller, then checks that the transaction fails. Then
  // checks that the storage item is not created.
  static constexpr ContractAddress kAddressCallee = make_contract_address(48);
  static constexpr ContractAddress kAddressCaller = make_contract_address(49);

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_callee;
    fns_callee.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          auto const &calldata = ctx.handler().get_calldata();
          if (calldata.size() != 1) {
            ADD_FAILURE() << "calldata should have size 1";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          auto key = make_storage_key("test");
          // Interpret calldata bytes as int8_t for set_value and add_value
          auto add_value = static_cast<int8_t>(calldata[0]);

          auto system_res =
              ctx.handler().set_nnint(key, std::nullopt, add_value,
                                      std::to_underlying(
                                          StorageDeltaFlags::None));

          if (!system_res.has_value()) {
            ADD_FAILURE() << "set_nnint should have succeeded";
            // System-level error during async operation
            return system_res.error().to_invoke_error();
          }

          auto const &syscall_res = system_res.value();
          if (!syscall_res.has_value()) {
            ADD_FAILURE() << "set_nnint should have succeeded";
            // Syscall itself reported an error (e.g., invalid arguments
            // for set_nnint)
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          return {}; // Success
        });
    auto native_contract_callee =
        std::make_unique<NativeContract>(std::move(fns_callee));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_caller;
    fns_caller.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          // Invoke callee method 0 in try_invoke frame with arguments [-5]
          Calldata args = {static_cast<uint8_t>(-5)};
          CopyableInvokeOutputs r0_outputs{};
          auto r0 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(kAddressCallee, args,
                                                   std::span<uint32_t>{},
                                                   inputs, r0_outputs);
          }();

          if (!r0.has_value()) {
            ADD_FAILURE() << "try_invoke should have failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          // The child frame ran, so the syscall returns
          // kSyscallErrorCodeNone; check that it failed (should fail because
          // 5 + -10 is negative), reported via frame_status != Return.
          if (r0.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
              r0_outputs.frame_status == InvocationFrameStatusCode::Return) {
            ADD_FAILURE() << "try_invoke should have failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          // Return a deterministic error to make the transaction fail
          return InvokeError{InvokeErrorCode::Deterministic};
        });
    auto native_contract_caller =
        std::make_unique<NativeContract>(std::move(fns_caller));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_callee));
    contract_map.emplace(1, std::move(native_contract_caller));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {kAddressCallee, kAddressCaller};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressCaller,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *context);

  auto res = handler.root_invoke();
  // The transaction should fail because the caller returns a deterministic
  // error (after observing that the try_invoke of the callee did not succeed).
  ASSERT_FALSE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  // The transaction should not commit because it failed
  ASSERT_TRUE(commit_res == ExecResult::ExecResultTxDeterministicError);

  advance_block();

  // Check that the storage item is not created (callee never committed a valid
  // nnint write)
  auto data_addrkey = make_contract_data_addrkey(kAddressCallee, "test");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_FALSE(data_item_opt.has_value());
}

TEST_P(TryInvokeTest, TryInvokeCrashWhileLocksHeld)
{
  // Test to make sure that the system does not crash when a transaction
  // is aborted while locks are held.
  // Asserts that bug like
  // https://github.com/groundhogchain/hogd/commit/f7a7b0f3737627a42814cea81923322c678df518
  // does not occur again.
  //
  // Writer contract — method 0: loads the calldata, then performs a bytestring
  // write of that calldata to a fixed key.
  //
  // Orchestrator contract:
  // 0: Invoke writer method 0 in a try_invoke frame, with argument "hello".
  // Then invokes writer method 0 again in try_invoke, with argument "world".
  // Both should succeed.
  // 1: Invoke writer method 0 in a try_invoke frame, with argument "foo".
  // Check that the try_invoke fails.
  // Then return a deterministic error.
  //
  // The test invokes orchestrator method 0, but does not immediately commit the
  // transaction.
  // Then it starts another transaction that invokes orchestrator method 1.
  // That second transaction should fail.
  // We then terminate the test without committing either transaction.
  // The test should not crash.

  static constexpr ContractAddress kAddressWriter = make_contract_address(53);
  static constexpr ContractAddress kAddressOrchestrator =
      make_contract_address(54);

  // Writer: method 0 — write calldata as bytestring to storage.
  // Orchestrator: 0 — try_invoke writer twice ("hello", "world"); 1 —
  // try_invoke writer with "foo" (exclusive lock), then return error.
  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_w;
    fns_w.emplace(0,
                  [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                    auto const &calldata = ctx.handler().get_calldata();
                    auto key = make_storage_key("test");
                    auto [res, _gas] = syscall_BytestringSet_inner(
                        ctx.handler(), key, std::as_bytes(std::span(calldata)),
                        std::to_underlying(StorageDeltaFlags::None));
                    if (!res.has_value() ||
                        res.value() !=
                            SyscallErrorCode::kSyscallErrorCodeNone) {
                      ADD_FAILURE() << "Failed to set bytestring in method 0";
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }
                    return {};
                  });
    auto native_contract_writer =
        std::make_unique<NativeContract>(std::move(fns_w));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_o;
    fns_o.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          // Try invoke writer method 0 with "hello"
          Calldata args_hello = {'h', 'e', 'l', 'l', 'o'};
          auto r0 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter, args_hello,
                                                   std::span<uint32_t>{},
                                                   inputs,
                                                   CopyableInvokeOutputs{});
          }();
          if (!r0.has_value() ||
              r0.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          // Try invoke writer method 0 with "world"
          Calldata args_world = {'w', 'o', 'r', 'l', 'd'};
          auto r1 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter, args_world,
                                                   std::span<uint32_t>{},
                                                   inputs,
                                                   CopyableInvokeOutputs{});
          }();
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          return {};
        });
    fns_o.emplace(
        1, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          // Try invoke writer method 0 with "foo" (should fail due to
          // exclusive lock)
          Calldata args_foo = {'f', 'o', 'o'};
          CopyableInvokeOutputs r0_outputs{};
          auto r0 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter, args_foo,
                                                   std::span<uint32_t>{},
                                                   inputs, r0_outputs);
          }();
          if (!r0.has_value()) {
            ADD_FAILURE() << "should not have system error";
            return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          // The child frame ran (lock acquisition failed and was
          // converted to a deterministic failure), so the syscall
          // returns kSyscallErrorCodeNone; check that it failed (due
          // to the exclusive lock), reported via frame_status !=
          // Return.
          if (r0.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
              r0_outputs.frame_status == InvocationFrameStatusCode::Return) {
            ADD_FAILURE() << "try_invoke should have failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          // Return a deterministic error to make the transaction fail
          return InvokeError{InvokeErrorCode::Deterministic};
        });
    auto native_contract_orchestrator =
        std::make_unique<NativeContract>(std::move(fns_o));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_writer));
    contract_map.emplace(1, std::move(native_contract_orchestrator));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {kAddressWriter, kAddressOrchestrator};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  // First transaction: invoke orchestrator method 0 (should succeed and acquire
  // exclusive lock on writer storage)
  auto handler1_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressOrchestrator,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler1 = make_new_system_call_handler(handler1_tx, *context);

  auto res1 = handler1.root_invoke();
  ASSERT_TRUE(res1.is_root_success());
  // Don't commit this transaction yet (keeps the exclusive lock)

  // Second transaction: invoke orchestrator method 1 (should fail due to
  // exclusive lock)
  auto handler2_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressOrchestrator,
                                     .entrypoint = entrypoint_from_method_id(1),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler2 = make_new_system_call_handler(handler2_tx, *context);

  auto res2 = handler2.root_invoke();
  ASSERT_FALSE(res2.is_root_success()); // Should return error
}

TEST_P(TryInvokeTest, TryInvokeOnExclusiveLock)
{
  // Test on try invoke where a transaction fails after a try_invoke fails.
  //
  // Writer contract — method 0: loads the calldata, then performs a bytestring
  // write of that calldata to a fixed key.
  //
  // Orchestrator contract:
  // 0: Invoke writer method 0 in a try_invoke frame, with argument "hello".
  // Then invokes writer method 0 again in try_invoke, with argument "world".
  // Both should succeed.
  // 1: Invoke writer method 0 in a try_invoke frame, with argument "foo".
  // Check that the try_invoke fails.
  // Then return a deterministic error.
  //
  // The test invokes orchestrator method 0, but does not immediately commit the
  // transaction.
  // Then it starts another transaction that invokes orchestrator method 1.
  // That second transaction should fail. We then commit the first transaction,
  // which should succeed.

  static constexpr ContractAddress kAddressWriter = make_contract_address(53);
  static constexpr ContractAddress kAddressOrchestrator =
      make_contract_address(54);

  // Writer: method 0 — write calldata as bytestring to storage.
  // Orchestrator: 0 — try_invoke writer twice ("hello", "world"); 1 —
  // try_invoke writer with "foo" (exclusive lock), then return error.
  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_w;
    fns_w.emplace(0,
                  [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                    auto const &calldata = ctx.handler().get_calldata();
                    auto key = make_storage_key("test");
                    auto [res, _gas] = syscall_BytestringSet_inner(
                        ctx.handler(), key, std::as_bytes(std::span(calldata)),
                        std::to_underlying(StorageDeltaFlags::None));
                    if (!res.has_value() ||
                        res.value() !=
                            SyscallErrorCode::kSyscallErrorCodeNone) {
                      ADD_FAILURE() << "Failed to set bytestring in method 0";
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }
                    return {};
                  });
    auto native_contract_writer =
        std::make_unique<NativeContract>(std::move(fns_w));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_o;
    fns_o.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          // Try invoke writer method 0 with "hello"
          Calldata args_hello = {'h', 'e', 'l', 'l', 'o'};
          auto r0 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter, args_hello,
                                                   std::span<uint32_t>{},
                                                   inputs,
                                                   CopyableInvokeOutputs{});
          }();
          if (!r0.has_value() ||
              r0.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          // Try invoke writer method 0 with "world"
          Calldata args_world = {'w', 'o', 'r', 'l', 'd'};
          auto r1 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter, args_world,
                                                   std::span<uint32_t>{},
                                                   inputs,
                                                   CopyableInvokeOutputs{});
          }();
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          return {};
        });
    fns_o.emplace(
        1, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          // Try invoke writer method 0 with "foo" (should fail due to
          // exclusive lock)
          Calldata args_foo = {'f', 'o', 'o'};
          CopyableInvokeOutputs r0_outputs{};
          auto r0 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter, args_foo,
                                                   std::span<uint32_t>{},
                                                   inputs, r0_outputs);
          }();
          if (!r0.has_value()) {
            ADD_FAILURE() << "should not have system error";
            return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          // The child frame ran (lock acquisition failed and was
          // converted to a deterministic failure), so the syscall
          // returns kSyscallErrorCodeNone; check that it failed (due
          // to the exclusive lock), reported via frame_status !=
          // Return.
          if (r0.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
              r0_outputs.frame_status == InvocationFrameStatusCode::Return) {
            ADD_FAILURE() << "try_invoke should have failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          // Return a deterministic error to make the transaction fail
          return InvokeError{InvokeErrorCode::Deterministic};
        });
    auto native_contract_orchestrator =
        std::make_unique<NativeContract>(std::move(fns_o));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_writer));
    contract_map.emplace(1, std::move(native_contract_orchestrator));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {kAddressWriter, kAddressOrchestrator};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  // First transaction: invoke orchestrator method 0 (should succeed and acquire
  // exclusive lock on writer storage)
  auto handler1_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressOrchestrator,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler1 = make_new_system_call_handler(handler1_tx, *context);

  auto res1 = handler1.root_invoke();
  ASSERT_TRUE(res1.is_root_success());
  // Don't commit this transaction yet (keeps the exclusive lock)

  // Second transaction: invoke orchestrator method 1 (should fail due to
  // exclusive lock)
  auto handler2_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressOrchestrator,
                                     .entrypoint = entrypoint_from_method_id(1),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler2 = make_new_system_call_handler(handler2_tx, *context);

  handler2.enable_tracing();
  auto res2 = handler2.root_invoke();
  ASSERT_FALSE(res2.is_root_success()); // Should return error

  auto commit_res2 = handler2.try_commit_transaction();
  // transaction should not commit
  ASSERT_TRUE(commit_res2 == ExecResult::ExecResultTxDeterministicError);

  // The failing try_invoke child frame's trace carries the 2pc conflict.
  auto const *trace2 = handler2.get_trace();
  ASSERT_NE(trace2, nullptr);
  SyscallTrace const *invoke_sc = nullptr;
  for (auto const &sc : trace2->system_calls) {
    if (sc.body.syscall_number() == SyscallNo::kInvoke) {
      invoke_sc = &sc;
      break;
    }
  }
  ASSERT_NE(invoke_sc, nullptr);
  auto const &invoke_trace = invoke_sc->body.invoke();
  ASSERT_NE(invoke_trace.outputs, nullptr);
  ASSERT_NE(invoke_trace.outputs->child_trace, nullptr);
  auto const &child_trace = *invoke_trace.outputs->child_trace;
  // HogDb proposal mode reports all conflicts in debug builds, first-only in
  // release builds. MockInMemory does not populate conflict diagnostics; skip
  // that assertion.
  if (GetParam() == TestDB::kHogDb) {
    if constexpr (is_debug_build()) {
      ASSERT_EQ(child_trace.commit_conflict.level(),
                DbDiagnosticLevel::kDbDiagnosticAll);
      bool found_excl = false;
      for (auto const &c : child_trace.commit_conflict.conflicts()) {
        if (c.code == DbConflictCode::kDbConflictExclusiveLock) {
          found_excl = true;
        }
      }
      ASSERT_TRUE(found_excl);
    }
    else {
      // First mode reports whichever conflicting delta hog_db hit first,
      // which is not deterministic when the batch can conflict more than
      // once, so don't assert a specific code.
      ASSERT_EQ(child_trace.commit_conflict.level(),
                DbDiagnosticLevel::kDbDiagnosticFirst);
    }
  }

  // Now commit the first transaction (should succeed)
  auto commit_res1 = handler1.try_commit_transaction();
  ASSERT_TRUE(commit_res1 == ExecResult::ExecResultOK);

  advance_block();

  // Check that the storage item was written with "world" (the last write from
  // orchestrator method 0)
  auto data_addrkey = make_contract_data_addrkey(kAddressWriter, "test");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_TRUE(data_item_opt.has_value());

  std::vector<uint8_t> world_data = {'w', 'o', 'r', 'l', 'd'};
  auto expect_span = std::as_bytes(std::span(world_data));
  Bytestring expect;
  expect.resize(expect_span.size());
  std::memcpy(expect.data(), expect_span.data(), expect_span.size());
  ASSERT_EQ(data_item_opt.value().bytestring(), expect);
}

TEST_P(TryInvokeTest, TryInvokeNNIntAddThenAbort)
{
  // Test on try invoke where a transaction fails after a try_invoke fails.
  //
  // Writer contract — method 0: loads the calldata, interprets it as one int8_t
  // value [a], and performs an nnint_add of (a) on a storage key.
  //
  // Orchestrator contract:
  // 0: Invokes writer method 0 in a try_invoke frame, with arguments [-5].
  // Check that that invocation failed, and then return a deterministic error.
  // 1: Invokes writer method 0 with arguments [1] (try_invoke).
  // Check that the invocation succeeds.
  //
  // The test invokes orchestrator method 1, but does not immediately commit the
  // transaction.
  // Then it starts another transaction that invokes orchestrator method 0.
  // That second transaction should fail. We then commit the first transaction,
  // which should succeed.

  static constexpr ContractAddress kAddressWriter = make_contract_address(50);
  static constexpr ContractAddress kAddressOrchestrator =
      make_contract_address(51);

  // Writer: method 0 — nnint_add from one calldata byte.
  // Orchestrator: 0 — try_invoke writer with [-5], then error; 1 — try_invoke
  // writer with [1], expect success.
  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_w;
    fns_w.emplace(0,
                  [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                    auto const &calldata = ctx.handler().get_calldata();
                    if (calldata.size() != 1) {
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }
                    auto key = make_storage_key("test");
                    // Interpret calldata bytes as int8_t for add_value
                    auto add_value = static_cast<int8_t>(calldata[0]);

                    auto system_res =
                        ctx.handler().set_nnint(key, std::nullopt, add_value,
                                                std::to_underlying(
                                                    StorageDeltaFlags::None));

                    if (!system_res.has_value()) {
                      // System-level error during async operation
                      return system_res.error().to_invoke_error();
                    }

                    auto const &syscall_res = system_res.value();
                    if (!syscall_res.has_value()) {
                      // Syscall itself reported an error (e.g., invalid
                      // arguments for set_nnint)
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }

                    return {}; // Success
                  });
    auto native_contract_writer =
        std::make_unique<NativeContract>(std::move(fns_w));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_o;
    fns_o.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          // Invoke writer method 0 in try_invoke frame with arguments
          // [-5]
          Calldata args = {static_cast<uint8_t>(-5)};
          CopyableInvokeOutputs r0_outputs{};
          auto r0 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter, args,
                                                   std::span<uint32_t>{},
                                                   inputs, r0_outputs);
          }();

          if (!r0.has_value()) {
            return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          // The child frame ran, so the syscall returns
          // kSyscallErrorCodeNone; check that the try_invoke failed
          // (should fail because -5 is negative), reported via
          // frame_status != Return.
          if (r0.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
              r0_outputs.frame_status == InvocationFrameStatusCode::Return) {
            return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          // Return a deterministic error to make the transaction fail
          return InvokeError{InvokeErrorCode::Deterministic};
        });
    fns_o.emplace(1,
                  [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                    // Invoke writer method 0 with arguments [1]
                    Calldata args = {1};
                    auto r0 = [&]() {
                      CopyableInvokeInputs inputs{
                          .entrypoint_id = 0,
                          .gas_limit = UINT64_MAX,
                          .flags = std::to_underlying(InvokeFlag::TryInvoke),
                      };
                      return ctx.syscall<SyscallNo::kInvoke>(
                          kAddressWriter, args, std::span<uint32_t>{}, inputs,
                          CopyableInvokeOutputs{});
                    }();

                    if (!r0.has_value()) {
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }

                    // Check that the invocation succeeds
                    if (r0.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }

                    return {};
                  });
    auto native_contract_orchestrator =
        std::make_unique<NativeContract>(std::move(fns_o));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_writer));
    contract_map.emplace(1, std::move(native_contract_orchestrator));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {kAddressWriter, kAddressOrchestrator};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  // First transaction: invoke orchestrator method 1 (should succeed; writes
  // nnint on writer)
  auto handler1_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressOrchestrator,
                                     .entrypoint = entrypoint_from_method_id(1),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler1 = make_new_system_call_handler(handler1_tx, *context);

  auto res1 = handler1.root_invoke();
  ASSERT_TRUE(res1.is_root_success());
  // Don't commit this transaction yet

  // Second transaction: invoke orchestrator method 0 (should fail)
  auto handler2_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressOrchestrator,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler2 = make_new_system_call_handler(handler2_tx, *context);

  auto res2 = handler2.root_invoke();
  ASSERT_FALSE(res2.is_root_success()); // Should return error

  auto commit_res2 = handler2.try_commit_transaction();
  // Transaction should not commit because handler2 returned a failure
  ASSERT_EQ(commit_res2, ExecResult::ExecResultTxDeterministicError);

  // Now commit the first transaction (should succeed)
  auto commit_res1 = handler1.try_commit_transaction();
  ASSERT_EQ(commit_res1, ExecResult::ExecResultOK);

  advance_block();

  // Check that the storage item was created with value 1 (from orchestrator
  // method 1)
  auto data_addrkey = make_contract_data_addrkey(kAddressWriter, "test");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_TRUE(data_item_opt.has_value());
  ASSERT_EQ(data_item_opt.value().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(data_item_opt.value().nnint()), 1);
}

TEST_P(TryInvokeTest, TryInvokeHandleDBFailure)
{
  // Test on try invoke responding to rejection from the database.

  // Writer contract — method 0: loads the calldata as two int8_t values [a, b],
  // and performs an nnint_set_add of (a, b) on a storage key.
  //
  // Orchestrator (two native registrations, one per test case, each targeting
  // its paired writer):
  // 0:
  // Load the calldata, and interpret it as two booleans [b1, b2].
  // Invoke writer method 0 with parameters (10, -5), in a try_invoke frame.
  // Then invoke writer method 0 with parameters (5, 10), in another try_invoke
  // frame.
  // Assert that b1 is true if and only if the first invocation of writer
  // method 0 succeeded. Assert that b2 is true if and only if the second
  // invocation of writer method 0 succeeded. Then return success.
  //
  // 1:
  // Load the calldata, and interpret it as a boolean.
  // Invoke writer method 0 with parameters (5, -5), in a try_invoke frame.
  // Assert that the try_invoke frame returns successfully if and only if the
  // input boolean is true. Then return success.

  static constexpr ContractAddress kWriterTc1 = make_contract_address(28);
  static constexpr ContractAddress kOrchTc1 = make_contract_address(29);
  static constexpr ContractAddress kWriterTc2 = make_contract_address(30);
  static constexpr ContractAddress kOrchTc2 = make_contract_address(31);

  auto make_orchestrator = [](ContractAddress writer_addr) {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_o;
    fns_o.emplace(
        0,
        [writer_addr](
            NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          auto const &calldata = ctx.handler().get_calldata();
          if (calldata.size() != 2) { // Expecting two booleans
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          bool expect_first_succeeds = static_cast<bool>(calldata[0]);
          bool expect_second_succeeds = static_cast<bool>(calldata[1]);

          Calldata args_first_invoke = {10, static_cast<uint8_t>(-5)};
          CopyableInvokeOutputs r0_outputs{};
          auto r0 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(writer_addr,
                                                   args_first_invoke,
                                                   std::span<uint32_t>{},
                                                   inputs, r0_outputs);
          }();

          if (!r0.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          // The child ran whenever the syscall returns kSyscallErrorCodeNone;
          // it committed iff frame_status == Return.
          bool first_succeeded =
              (r0.value() == SyscallErrorCode::kSyscallErrorCodeNone &&
               r0_outputs.frame_status == InvocationFrameStatusCode::Return);
          if (first_succeeded != expect_first_succeeds) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          Calldata args_second_invoke = {5, 10};
          CopyableInvokeOutputs r1_outputs{};
          auto r1 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(writer_addr,
                                                   args_second_invoke,
                                                   std::span<uint32_t>{},
                                                   inputs, r1_outputs);
          }();

          if (!r1.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          // The child ran whenever the syscall returns kSyscallErrorCodeNone;
          // it committed iff frame_status == Return.
          bool second_succeeded =
              (r1.value() == SyscallErrorCode::kSyscallErrorCodeNone &&
               r1_outputs.frame_status == InvocationFrameStatusCode::Return);
          if (second_succeeded != expect_second_succeeds) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          return {};
        });
    fns_o.emplace(
        1,
        [writer_addr](
            NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          auto const &calldata = ctx.handler().get_calldata();
          if (calldata.size() != 1) { // Expecting one boolean
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          bool expect_invoke_succeeds = static_cast<bool>(calldata[0]);

          Calldata args_invoke = {5, static_cast<uint8_t>(-5)};
          CopyableInvokeOutputs r0_outputs{};
          auto r0 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(writer_addr, args_invoke,
                                                   std::span<uint32_t>{},
                                                   inputs, r0_outputs);
          }();

          if (!r0.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          // The child ran whenever the syscall returns kSyscallErrorCodeNone;
          // it committed iff frame_status == Return.
          bool invoke_succeeded =
              (r0.value() == SyscallErrorCode::kSyscallErrorCodeNone &&
               r0_outputs.frame_status == InvocationFrameStatusCode::Return);
          if (invoke_succeeded != expect_invoke_succeeds) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    return std::make_unique<NativeContract>(std::move(fns_o));
  };

  auto make_context = [&]() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_w;
    fns_w.emplace(0,
                  [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                    auto const &calldata = ctx.handler().get_calldata();
                    if (calldata.size() != 2) {
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }
                    auto key = make_storage_key("test");
                    // Interpret calldata bytes as int8_t for set_value and
                    // add_value
                    auto set_value = static_cast<int8_t>(calldata[0]);
                    auto add_value = static_cast<int8_t>(calldata[1]);

                    auto system_res =
                        ctx.handler().set_nnint(key, set_value, add_value,
                                                std::to_underlying(
                                                    StorageDeltaFlags::None));

                    if (!system_res.has_value()) {
                      // System-level error during async operation
                      return system_res.error().to_invoke_error();
                    }

                    auto const &syscall_res = system_res.value();
                    if (!syscall_res.has_value()) {
                      // Syscall itself reported an error (e.g., invalid
                      // arguments for set_nnint)
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }

                    return {}; // Success
                  });
    auto native_contract_writer =
        std::make_unique<NativeContract>(std::move(fns_w));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_writer));
    contract_map.emplace(1, make_orchestrator(kWriterTc1));
    contract_map.emplace(2, make_orchestrator(kWriterTc2));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  auto writer_template = make_native_contract(0);
  auto orch_tc1_template = make_native_contract(1);
  auto orch_tc2_template = make_native_contract(2);
  write_contract(writer_template);
  write_contract(orch_tc1_template);
  write_contract(orch_tc2_template);

  // Test case 1:
  // Test that try invoke can acquire an exclusive lock.
  write_deployment(kWriterTc1, writer_template);
  write_deployment(kOrchTc1, orch_tc1_template);
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(kWriterTc1, 1'000'000'000);
  setup_grant_storage_token(kOrchTc1, 1'000'000'000);

  // TC1.1: First, invoke orchestrator method 0 with parameters [true, true].
  // This should succeed. This should leave, in the database, an exclusive lock
  // on the item.
  auto tc1_handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = kOrchTc1,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {1, 1}},
          .uniquifier = make_new_uniquifier(),
      }};
  SystemCallHandler tc1_handler =
      make_new_system_call_handler(tc1_handler_tx, *context);
  {
    auto res = tc1_handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
  }

  // TC1.2: Then, in the same block, before committing the first transaction,
  // create a second transaction that invokes orchestrator method 1 with
  // parameter [false].
  // This should succeed (the try invoke fails, because the first tx still holds
  // the exclusive lock).
  auto tc1_handler2_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = kOrchTc1,
                                   .entrypoint = entrypoint_from_method_id(1),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {0}},
          .uniquifier = make_new_uniquifier(),
      }};
  SystemCallHandler tc1_handler2 =
      make_new_system_call_handler(tc1_handler2_tx, *context);
  {
    auto res = tc1_handler2.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = tc1_handler2.try_commit_transaction();
    ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  }

  // TC1.3: Then commit the first transaction (releasing the exclusive lock to a
  // set_typeclass lock).
  {
    auto commit_res = tc1_handler.try_commit_transaction();
    ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  }

  // TC1.4: Then invoke orchestrator method 1 again, with parameter [true]. This
  // should succeed.
  auto tc1_handler3_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = kOrchTc1,
                                   .entrypoint = entrypoint_from_method_id(1),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {1}},
          .uniquifier = make_new_uniquifier(),
      }};
  SystemCallHandler tc1_handler3 =
      make_new_system_call_handler(tc1_handler3_tx, *context);
  {
    auto res = tc1_handler3.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = tc1_handler3.try_commit_transaction();
    ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
    ASSERT_TRUE(tc1_handler3.emit_events(ExecResult::ExecResultOK,
                                         ProposedTransaction{
                                             .signed_tx = tc1_handler3_tx})());
  }

  advance_block();

  // Test case 2:
  // Test that try invoke can handle a rejection from the database.
  // First, invoke orchestrator method 1, with parameter [true]. This should
  // succeed.
  // Commit the first transaction.
  // Then, invoke orchestrator method 0 with parameters [false, true]. This
  // should succeed.
  // Commit the second transaction.
  write_deployment(kWriterTc2, writer_template);
  write_deployment(kOrchTc2, orch_tc2_template);
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(kWriterTc2, 1'000'000'000);
  setup_grant_storage_token(kOrchTc2, 1'000'000'000);

  // Step 1 & 2: Invoke orchestrator method 1 ({true}) and commit.
  auto tc2_handler1_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = kOrchTc2,
                                   .entrypoint = entrypoint_from_method_id(1),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {1}}, // true
          .uniquifier = make_new_uniquifier(),
      }};
  SystemCallHandler tc2_handler1 =
      make_new_system_call_handler(tc2_handler1_tx, *context);
  {
    auto res = tc2_handler1.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = tc2_handler1.try_commit_transaction();
    ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
    ASSERT_TRUE(tc2_handler1.emit_events(ExecResult::ExecResultOK,
                                         ProposedTransaction{
                                             .signed_tx = tc2_handler1_tx})());
  }

  // Step 3 & 4: Invoke orchestrator method 0 ({false, true}) and commit.
  auto tc2_handler2_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = kOrchTc2,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {0, 1}}, // {false, true}
          .uniquifier = make_new_uniquifier(),
      }};
  SystemCallHandler tc2_handler2 =
      make_new_system_call_handler(tc2_handler2_tx, *context);
  {
    auto res = tc2_handler2.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = tc2_handler2.try_commit_transaction();
    ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
    ASSERT_TRUE(tc2_handler2.emit_events(ExecResult::ExecResultOK,
                                         ProposedTransaction{
                                             .signed_tx = tc2_handler2_tx})());
  }

  advance_block();
}

TEST_P(TryInvokeTest, StorageCostAccounting)
{
  // Test that checks that storage costs are accounted for correctly in try
  // invoke. The writer contract has enough storage tokens for one bytestring
  // write. The root frame invokes a try_invoke frame on the writer, which
  // attempts to write two bytestrings. That frame fails. The root frame then
  // invokes a try_invoke frame on the writer, which makes one bytestring write.
  // That frame succeeds. After commit, the writer's storage token balance
  // should be zero and only the first key should exist. Storage tokens are
  // granted only to the writer (the contract that owns the storage), not the
  // orchestrator.

  static constexpr ContractAddress kAddressWriter = make_contract_address(42);
  static constexpr ContractAddress kAddressOrchestrator =
      make_contract_address(41);

  // Bytestrings
  auto str_abc = make_bytestring("abc");
  auto str_def = make_bytestring("def");

  auto const *cost_model = get_default_cost_model();

  // Cost for one bytestring write ("abc")
  int128_t one_write_cost = cost_model->get_bytestring_cost(str_abc.size());

  // Writer: 0 — write "abc" to key0; 1 — write "abc" to key0 and "def" to key1.
  // Orchestrator: 0 — try_invoke writer 1 (fails), then try_invoke writer 0
  // (succeeds).
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_w;
  fns_w.emplace(
      0, [str_abc](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key0 = make_storage_key("test0");

        auto res = ctx.syscall<SyscallNo::kBytestringSet>(
            key0, std::as_bytes(std::span(str_abc)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "StorageCostAccounting: Failed to set bytestring in "
                           "writer method 0";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  fns_w.emplace(1,
                [str_abc, str_def](
                    NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto key0 = make_storage_key("test0");
                  auto key1 = make_storage_key("test1");
                  auto res0 = ctx.syscall<SyscallNo::kBytestringSet>(
                      key0, std::as_bytes(std::span(str_abc)),
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!res0.has_value() ||
                      res0.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE() << "StorageCostAccounting: Failed to set "
                                     "bytestring in writer method 1 (key0)";
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  auto res1 = ctx.syscall<SyscallNo::kBytestringSet>(
                      key1, std::as_bytes(std::span(str_def)),
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!res1.has_value() ||
                      res1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE() << "StorageCostAccounting: Failed to set "
                                     "bytestring in writer method 1 (key1)";
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  return {};
                });
  auto native_contract_writer =
      std::make_unique<NativeContract>(std::move(fns_w));
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_o;
  fns_o.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // First, try_invoke writer method 1 (should fail)
        CopyableInvokeOutputs r1_outputs{};
        auto r1 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 1,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r1_outputs);
        }();
        if (!r1.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // The child ran, so the syscall returns kSyscallErrorCodeNone; it
        // should have rolled back (not enough tokens for two writes), reported
        // via frame_status != Return.
        if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            r1_outputs.frame_status == InvocationFrameStatusCode::Return) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // Then, try_invoke writer method 0 (should succeed)
        CopyableInvokeOutputs r0_outputs{};
        auto r0 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r0_outputs);
        }();
        // The child should have run and committed, reported via
        // frame_status == Return.
        if (!r0.has_value() ||
            r0.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            r0_outputs.frame_status != InvocationFrameStatusCode::Return) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto native_contract_orchestrator =
      std::make_unique<NativeContract>(std::move(fns_o));
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_writer));
  contract_map.emplace(1, std::move(native_contract_orchestrator));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy writer and orchestrator; grant storage tokens only on the writer.
  auto writer_xdr = make_native_contract(0);
  auto orchestrator_xdr = make_native_contract(1);
  write_contract(writer_xdr);
  write_contract(orchestrator_xdr);
  write_deployment(kAddressWriter, writer_xdr);
  write_deployment(kAddressOrchestrator, orchestrator_xdr);
  setup_grant_storage_token(kAddressWriter, one_write_cost);
  // The orchestrator is the root invoked address, so its one committed tx
  // charges the replay-cache cost to it. Grant exactly that so the commit can
  // pay for it; the writer's asserted balance is unaffected.
  setup_grant_storage_token(kAddressOrchestrator, replay_cache_cost_charged(1));

  // Single transaction: invoke orchestrator method 0 (root)
  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressOrchestrator,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  // 3. Check writer storage token balance is zero
  AddressAndKey token_addrkey =
      make_free_native_token_balance_addrkey(kAddressWriter);
  auto token_item_opt = lookup_storage_item_from_db(token_addrkey);
  // assert that the free token balance item exists and has value 0
  ASSERT_TRUE(token_item_opt.has_value());
  ASSERT_EQ(token_item_opt.value().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(token_item_opt.value().nnint()), 0);

  // 4. Check that only key0 exists on the writer and has value "abc"
  auto data_addrkey0 = make_contract_data_addrkey(kAddressWriter, "test0");
  auto data_item0_opt = lookup_storage_item_from_db(data_addrkey0);
  ASSERT_TRUE(data_item0_opt.has_value());
  const auto &data_item0 = data_item0_opt.value();
  ASSERT_EQ(data_item0.bytestring(), str_abc);

  auto data_addrkey1 = make_contract_data_addrkey(kAddressWriter, "test1");
  auto data_item1_opt = lookup_storage_item_from_db(data_addrkey1);
  ASSERT_FALSE(data_item1_opt.has_value());
}

TEST_P(TryInvokeTest, MarginalCostAccounting)
{
  // Test to demonstrate necessity for memoizing locked costs.
  // Root frame invokes three try_invoke frames on the writer. Each one writes a
  // bytestring on separate keys, but the writer only has enough storage tokens
  // for two and a half. As such, the first and second try_invokes will succeed,
  // and the third will fail.
  //
  // At the end, the writer's storage token balance should be half a bytestring
  // write. Tokens are granted only to the writer, not the orchestrator.

  static constexpr ContractAddress kAddressWriter = make_contract_address(43);
  static constexpr ContractAddress kAddressOrchestrator =
      make_contract_address(40);

  // Bytestring and cost
  auto str_abc = make_bytestring("abc");
  auto str_def = make_bytestring("def");
  auto str_ghi = make_bytestring("ghi");

  auto const *cost_model = get_default_cost_model();
  int128_t one_write_cost = cost_model->get_bytestring_cost(str_abc.size());
  int128_t half_write_cost = one_write_cost / 2;
  int128_t total_tokens = (one_write_cost * 2) + half_write_cost;

  // Writer: 0 — "abc" to key0; 1 — "def" to key1; 2 — "ghi" to key2.
  // Orchestrator: 0 — try_invoke writer 0, then 1, then 2 (third fails).
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_w;
  fns_w.emplace(0,
                [str_abc](
                    NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto key0 = make_storage_key("test0");
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key0, std::as_bytes(std::span(str_abc)),
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE() << "MarginalCostAccounting: Failed to set "
                                     "bytestring in writer method 0";
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  return {};
                });
  fns_w.emplace(1,
                [str_def](
                    NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto key1 = make_storage_key("test1");
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key1, std::as_bytes(std::span(str_def)),
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE() << "MarginalCostAccounting: Failed to set "
                                     "bytestring in writer method 1";
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  return {};
                });
  fns_w.emplace(2,
                [str_ghi](
                    NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto key2 = make_storage_key("test2");
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key2, std::as_bytes(std::span(str_ghi)),
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE() << "MarginalCostAccounting: Failed to set "
                                     "bytestring in writer method 2";
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  return {};
                });
  auto native_contract_writer =
      std::make_unique<NativeContract>(std::move(fns_w));
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_o;
  fns_o.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Try invoke writer method 0
        CopyableInvokeOutputs r0_outputs{};
        auto r0 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r0_outputs);
        }();
        // The child should have run and committed, reported via
        // frame_status == Return.
        if (!r0.has_value() ||
            r0.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            r0_outputs.frame_status != InvocationFrameStatusCode::Return) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // Try invoke writer method 1
        CopyableInvokeOutputs r1_outputs{};
        auto r1 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 1,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r1_outputs);
        }();
        // The child should have run and committed, reported via
        // frame_status == Return.
        if (!r1.has_value() ||
            r1.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            r1_outputs.frame_status != InvocationFrameStatusCode::Return) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // Try invoke writer method 2 (should fail)
        CopyableInvokeOutputs r2_outputs{};
        auto r2 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 2,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r2_outputs);
        }();
        if (!r2.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // The child ran, so the syscall returns kSyscallErrorCodeNone; it
        // should have rolled back (not enough tokens for third write),
        // reported via frame_status != Return.
        if (r2.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            r2_outputs.frame_status == InvocationFrameStatusCode::Return) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto native_contract_orchestrator =
      std::make_unique<NativeContract>(std::move(fns_o));
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_writer));
  contract_map.emplace(1, std::move(native_contract_orchestrator));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy writer and orchestrator; grant storage tokens only on the writer.
  auto writer_xdr = make_native_contract(0);
  auto orchestrator_xdr = make_native_contract(1);
  write_contract(writer_xdr);
  write_contract(orchestrator_xdr);
  write_deployment(kAddressWriter, writer_xdr);
  write_deployment(kAddressOrchestrator, orchestrator_xdr);
  setup_grant_storage_token(kAddressWriter, total_tokens);
  // The orchestrator is the root invoked address, so its one committed tx
  // charges the replay-cache cost to it. Grant exactly that so the commit can
  // pay for it; the writer's asserted balance is unaffected.
  setup_grant_storage_token(kAddressOrchestrator, replay_cache_cost_charged(1));

  // Single transaction: invoke orchestrator method 0 (root)
  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressOrchestrator,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  // Check writer storage token balance is half a write
  AddressAndKey token_addrkey = {.address = kAddressWriter,
                                 .key = StorageKey(
                                     StorageKeyType::FreeNativeTokenBalance)};
  auto token_item_opt = lookup_storage_item_from_db(token_addrkey);
  ASSERT_TRUE(token_item_opt.has_value());
  ASSERT_EQ(token_item_opt.value().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(token_item_opt.value().nnint()), half_write_cost);

  // Check that key0 and key1 exist on the writer and have value "abc" and "def"
  auto data_addrkey0 = make_contract_data_addrkey(kAddressWriter, "test0");
  auto data_item0_opt = lookup_storage_item_from_db(data_addrkey0);
  ASSERT_TRUE(data_item0_opt.has_value());
  ASSERT_EQ(data_item0_opt.value().bytestring(), str_abc);

  auto data_addrkey1 = make_contract_data_addrkey(kAddressWriter, "test1");
  auto data_item1_opt = lookup_storage_item_from_db(data_addrkey1);
  ASSERT_TRUE(data_item1_opt.has_value());
  ASSERT_EQ(data_item1_opt.value().bytestring(), str_def);

  // Check that key2 does not exist on the writer
  auto data_addrkey2 = make_contract_data_addrkey(kAddressWriter, "test2");
  auto data_item2_opt = lookup_storage_item_from_db(data_addrkey2);
  ASSERT_FALSE(data_item2_opt.has_value());
}

TEST_P(TryInvokeTest, CostChangeAccounting)
{
  // Test to check how marginal costs are accounted for when a try_invoke
  // updates, not creates, an item.
  // Root invocation makes three try_invoke invocations on the writer.
  // The first two succeed, and the third fails.
  // The first makes a short bytestring write.
  // The second makes a medium bytestring write.
  // The third makes a long bytestring write.
  // The writer has enough storage tokens for the first two writes, but not the
  // third. Storage tokens are granted only to the writer, not the orchestrator.

  static constexpr ContractAddress kAddressWriter = make_contract_address(44);
  static constexpr ContractAddress kAddressOrchestrator =
      make_contract_address(39);

  // Bytestrings
  auto str_short = make_bytestring("abc");            // 3 bytes
  auto str_medium = make_bytestring("abcdef");        // 6 bytes
  auto str_long = make_bytestring("abcdefghijklmno"); // 15 bytes

  auto const *cost_model = get_default_cost_model();

  // Cost for each update
  int128_t cost_short = cost_model->get_bytestring_cost(str_short.size());
  int128_t cost_medium = cost_model->get_bytestring_cost(str_medium.size());

  // Marginal cost for updating from short to medium
  int128_t marginal_medium = cost_medium - cost_short;

  // Grant enough tokens for the first two updates (create + update), but not
  // the third
  int128_t total_tokens = cost_short + marginal_medium;

  // Writer: 0 — short to key0; 1 — medium to key0; 2 — long to key0.
  // Orchestrator: 0 — try_invoke writer 0, then 1, then 2 (third fails).
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_w;
  fns_w.emplace(
      0, [str_short](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key0 = make_storage_key("test0");
        auto [res, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), key0, std::as_bytes(std::span(str_short)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "CostChangeAccounting: Failed to set bytestring "
                           "in writer method 0";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  fns_w.emplace(1,
                [str_medium](
                    NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto key0 = make_storage_key("test0");
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key0, std::as_bytes(std::span(str_medium)),
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE()
                        << "CostChangeAccounting: Failed to set bytestring "
                           "in writer method 1";
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  return {};
                });
  fns_w.emplace(
      2, [str_long](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key0 = make_storage_key("test0");
        auto [res, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), key0, std::as_bytes(std::span(str_long)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "CostChangeAccounting: Failed to set bytestring "
                           "in writer method 2";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto native_contract_writer =
      std::make_unique<NativeContract>(std::move(fns_w));
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_o;
  fns_o.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Try invoke writer method 0 (short)
        CopyableInvokeOutputs r0_outputs{};
        auto r0 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r0_outputs);
        }();
        // The child should have run and committed, reported via
        // frame_status == Return.
        if (!r0.has_value() ||
            r0.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            r0_outputs.frame_status != InvocationFrameStatusCode::Return) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // Try invoke writer method 1 (medium)
        CopyableInvokeOutputs r1_outputs{};
        auto r1 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 1,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r1_outputs);
        }();
        // The child should have run and committed, reported via
        // frame_status == Return.
        if (!r1.has_value() ||
            r1.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            r1_outputs.frame_status != InvocationFrameStatusCode::Return) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // Try invoke writer method 2 (long) -- should fail
        CopyableInvokeOutputs r2_outputs{};
        auto r2 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 2,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r2_outputs);
        }();
        if (!r2.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // The child ran, so the syscall returns kSyscallErrorCodeNone; it
        // should have rolled back (expects an error), reported via
        // frame_status != Return.
        if (r2.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            r2_outputs.frame_status == InvocationFrameStatusCode::Return) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto native_contract_orchestrator =
      std::make_unique<NativeContract>(std::move(fns_o));
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_writer));
  contract_map.emplace(1, std::move(native_contract_orchestrator));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy writer and orchestrator; grant storage tokens only on the writer.
  auto writer_xdr = make_native_contract(0);
  auto orchestrator_xdr = make_native_contract(1);
  write_contract(writer_xdr);
  write_contract(orchestrator_xdr);
  write_deployment(kAddressWriter, writer_xdr);
  write_deployment(kAddressOrchestrator, orchestrator_xdr);
  setup_grant_storage_token(kAddressWriter, total_tokens);
  // The orchestrator is the root invoked address, so its one committed tx
  // charges the replay-cache cost to it. Grant exactly that so the commit can
  // pay for it; the writer's asserted balance is unaffected.
  setup_grant_storage_token(kAddressOrchestrator, replay_cache_cost_charged(1));

  // Single transaction: invoke orchestrator method 0 (root)
  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressOrchestrator,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  // Check writer storage token balance is zero
  AddressAndKey token_addrkey = {.address = kAddressWriter,
                                 .key = StorageKey(
                                     StorageKeyType::FreeNativeTokenBalance)};
  auto token_item_opt = lookup_storage_item_from_db(token_addrkey);
  ASSERT_TRUE(token_item_opt.has_value());
  ASSERT_EQ(token_item_opt.value().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(token_item_opt.value().nnint()), 0);

  // Check that key0 exists on the writer and has value "abcdef"
  auto data_addrkey0 = make_contract_data_addrkey(kAddressWriter, "test0");
  auto data_item0_opt = lookup_storage_item_from_db(data_addrkey0);
  ASSERT_TRUE(data_item0_opt.has_value());
  ASSERT_EQ(data_item0_opt.value().bytestring(), str_medium);
}

TEST_P(TryInvokeTest, CostChangeDecreaseSuccessCase)
{
  // Test to make sure that if the cost of an item decreases, the storage token
  // balance is calculated correctly. Root invocation makes two try_invoke
  // invocations on the writer. Both succeed. The first makes a long bytestring
  // write. The second makes a short bytestring write. The writer has enough
  // storage tokens for both writes. The end of the test asserts that the
  // writer's storage token balance was charged for the short write. Storage
  // tokens are granted only to the writer, not the orchestrator.

  static constexpr ContractAddress kAddressWriter = make_contract_address(45);
  static constexpr ContractAddress kAddressOrchestrator =
      make_contract_address(38);

  // Bytestrings
  auto str_long = make_bytestring("abcdefghijklmno"); // 15 bytes
  auto str_short = make_bytestring("abc");            // 3 bytes

  auto const *cost_model = get_default_cost_model();

  // Costs
  int128_t cost_long = cost_model->get_bytestring_cost(str_long.size());
  int128_t cost_short = cost_model->get_bytestring_cost(str_short.size());

  // Grant enough tokens for the long write.
  int128_t total_tokens = cost_long;

  // Writer: 0 — long string to key0; 1 — short string to key0.
  // Orchestrator: 0 — try_invoke writer 0 (long), then try_invoke writer 1
  // (short).
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_w;
  fns_w.emplace(
      0, [str_long](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key0 = make_storage_key("test0");
        auto [res, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), key0, std::as_bytes(std::span(str_long)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "CostChangeDecreaseSuccessCase: Failed to set "
                           "bytestring in writer method 0";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  fns_w.emplace(
      1, [str_short](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key0 = make_storage_key("test0");
        auto [res, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), key0, std::as_bytes(std::span(str_short)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "CostChangeDecreaseSuccessCase: Failed to set "
                           "bytestring in writer method 1";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto native_contract_writer =
      std::make_unique<NativeContract>(std::move(fns_w));
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_o;
  fns_o.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Try invoke writer method 0 (long)
        auto r0 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r0.has_value() ||
            r0.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // Try invoke writer method 1 (short)
        auto r1 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 1,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r1.has_value() ||
            r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto native_contract_orchestrator =
      std::make_unique<NativeContract>(std::move(fns_o));
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_writer));
  contract_map.emplace(1, std::move(native_contract_orchestrator));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy writer and orchestrator; grant storage tokens only on the writer.
  auto writer_xdr = make_native_contract(0);
  auto orchestrator_xdr = make_native_contract(1);
  write_contract(writer_xdr);
  write_contract(orchestrator_xdr);
  write_deployment(kAddressWriter, writer_xdr);
  write_deployment(kAddressOrchestrator, orchestrator_xdr);
  setup_grant_storage_token(kAddressWriter, total_tokens);
  // The orchestrator is the root invoked address, so its one committed tx
  // charges the replay-cache cost to it. Grant exactly that so the commit can
  // pay for it; the writer's asserted balance is unaffected.
  setup_grant_storage_token(kAddressOrchestrator, replay_cache_cost_charged(1));

  // Single transaction: invoke orchestrator method 0 (root)
  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressOrchestrator,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  // Check writer storage token balance
  AddressAndKey token_addrkey = {.address = kAddressWriter,
                                 .key = StorageKey(
                                     StorageKeyType::FreeNativeTokenBalance)};
  auto token_item_opt = lookup_storage_item_from_db(token_addrkey);
  ASSERT_TRUE(token_item_opt.has_value());
  ASSERT_EQ(token_item_opt.value().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  // Initial tokens was cost_long. Final cost is cost_short.
  // The balance should be total_tokens - cost_short.
  int128_t expected_balance = total_tokens - cost_short;
  EXPECT_EQ(from_xdr(token_item_opt.value().nnint()), expected_balance);

  // Check that key0 exists on the writer and has value from the short write.
  auto data_addrkey0 = make_contract_data_addrkey(kAddressWriter, "test0");
  auto data_item0_opt = lookup_storage_item_from_db(data_addrkey0);
  ASSERT_TRUE(data_item0_opt.has_value());
  ASSERT_EQ(data_item0_opt.value().bytestring(), str_short);
}

TEST_P(TryInvokeTest, CostChangeDecreaseFailCase)
{
  // Test to make sure that if the cost of an item decreases, the storage token
  // balance is calculated correctly.
  // Root invocation first makes a try_invocation into the writer that performs
  // a long bytestring write.
  // Then it makes a regular invocation into a separate contract.
  // That contract try_invokes the writer's short write, then returns an error.
  // The root orchestrator expects that error and finishes successfully.
  // Storage tokens are granted only to the writer.

  static constexpr ContractAddress kAddressWriter = make_contract_address(46);
  // Intermediate contract (old method 2): try_invoke writer short write, then
  // returns error so that frame's effects roll back.
  static constexpr ContractAddress kAddressTryInvokeThenError =
      make_contract_address(37);
  static constexpr ContractAddress kAddressOrchestrator =
      make_contract_address(36);

  // Bytestrings
  auto str_long = make_bytestring("abcdefghijklmno"); // 15 bytes
  auto str_short = make_bytestring("abc");            // 3 bytes

  auto const *cost_model = get_default_cost_model();

  // Costs
  int128_t cost_long = cost_model->get_bytestring_cost(str_long.size());

  // Grant enough tokens for the long write.
  int128_t total_tokens = cost_long;

  // Writer: 0 — long string to key0; 1 — short string to key0.
  // kAddressTryInvokeThenError: 0 — try_invoke writer 1, then error.
  // Orchestrator: 0 — try_invoke writer 0, then regular invoke
  // kAddressTryInvokeThenError:0 (expects failure).
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_w;
  fns_w.emplace(
      0, [str_long](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key0 = make_storage_key("test0");
        auto res = ctx.syscall<SyscallNo::kBytestringSet>(
            key0, std::as_bytes(std::span(str_long)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "CostChangeDecreaseFailCase: Failed to set "
                           "bytestring in writer method 0";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  fns_w.emplace(
      1, [str_short](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key0 = make_storage_key("test0");
        auto res = ctx.syscall<SyscallNo::kBytestringSet>(
            key0, std::as_bytes(std::span(str_short)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "CostChangeDecreaseFailCase: Failed to set "
                           "bytestring in writer method 1";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto native_contract_writer =
      std::make_unique<NativeContract>(std::move(fns_w));
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_tie;
  fns_tie.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Try invoke writer method 1 (short write)
        CopyableInvokeOutputs r1_outputs{};
        auto r1 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 1,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r1_outputs);
        }();
        // The child should have run and committed, reported via
        // frame_status == Return.
        if (!r1.has_value() ||
            r1.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            r1_outputs.frame_status != InvocationFrameStatusCode::Return) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // Now, return an error. This will cause this frame's
        // effects to be rolled back.
        return InvokeError{InvokeErrorCode::Deterministic};
      });
  auto native_contract_try_invoke_then_error =
      std::make_unique<NativeContract>(std::move(fns_tie));
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_o;
  fns_o.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Try invoke writer method 0 (long write)
        CopyableInvokeOutputs r0_outputs{};
        auto r0 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r0_outputs);
        }();
        // The child should have run and committed, reported via
        // frame_status == Return.
        if (!r0.has_value() ||
            r0.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            r0_outputs.frame_status != InvocationFrameStatusCode::Return) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Regular invoke on intermediate contract (old method 2)
        CopyableInvokeOutputs r2_outputs{};
        auto r2 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressTryInvokeThenError,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r2_outputs);
        }();
        if (!r2.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // The child ran, so the syscall returns kSyscallErrorCodeNone; we
        // expect the intermediate to have failed (it returns a deterministic
        // error), reported via frame_status != Return.
        if (r2.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            r2_outputs.frame_status == InvocationFrameStatusCode::Return) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto native_contract_orchestrator =
      std::make_unique<NativeContract>(std::move(fns_o));
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_writer));
  contract_map.emplace(1, std::move(native_contract_try_invoke_then_error));
  contract_map.emplace(2, std::move(native_contract_orchestrator));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy writer, intermediate, and orchestrator; grant storage tokens only on
  // the writer.
  auto writer_xdr = make_native_contract(0);
  auto try_invoke_then_error_xdr = make_native_contract(1);
  auto orchestrator_xdr = make_native_contract(2);
  write_contract(writer_xdr);
  write_contract(try_invoke_then_error_xdr);
  write_contract(orchestrator_xdr);
  write_deployment(kAddressWriter, writer_xdr);
  write_deployment(kAddressTryInvokeThenError, try_invoke_then_error_xdr);
  write_deployment(kAddressOrchestrator, orchestrator_xdr);
  setup_grant_storage_token(kAddressWriter, total_tokens);
  // The orchestrator is the root invoked address, so its one committed tx
  // charges the replay-cache cost to it. Grant exactly that so the commit can
  // pay for it; the writer's asserted balance is unaffected.
  setup_grant_storage_token(kAddressOrchestrator, replay_cache_cost_charged(1));

  // Single transaction: invoke orchestrator method 0 (root)
  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressOrchestrator,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  // Check writer storage token balance
  AddressAndKey token_addrkey = {.address = kAddressWriter,
                                 .key = StorageKey(
                                     StorageKeyType::FreeNativeTokenBalance)};
  auto token_item_opt = lookup_storage_item_from_db(token_addrkey);
  ASSERT_TRUE(token_item_opt.has_value());
  ASSERT_EQ(token_item_opt.value().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  // The cost of the short write was rolled back.
  // Final cost is cost_long. Initial tokens was cost_long.
  // So the balance should be 0.
  EXPECT_EQ(from_xdr(token_item_opt.value().nnint()), 0);

  // Check that key0 exists on the writer and has value from the long write.
  auto data_addrkey0 = make_contract_data_addrkey(kAddressWriter, "test0");
  auto data_item0_opt = lookup_storage_item_from_db(data_addrkey0);
  ASSERT_TRUE(data_item0_opt.has_value());
  ASSERT_EQ(data_item0_opt.value().bytestring(), str_long);
}

namespace {
template<typename T>
T
check_result(HostFnStatus<SyscallStatus<std::optional<T>>> res)
{
  if (!res.has_value() || !res.value().has_value() ||
      !res.value().value().has_value()) {
    panic("Failed to invoke method");
  }
  return res.value().value().value();
};
} // namespace

TEST_P(TryInvokeTest, TransferAndUseStorageWithinTransaction)
{
  if (GetParam() == TestDB::kMockInMemory) {
    GTEST_SKIP()
        << "MockDB is broken when having manual transfers + storage costs";
  }
  // Test to make sure that if a contract transfers storage tokens to another
  // contract, the new storage tokens can be used at the receiver contract
  // within the same transaction.

  // Test creates two NativeContracts, and deploys them at different addresses.
  // The first contract transfers storage tokens to the second contract.
  // The second contract then uses the storage tokens to write a bytestring.

  // This requires building two native contracts, and then deploying them at
  // separate addresses. Transfers should be done by invoking the
  // preset_native_token contract.

  constexpr uint32_t kSenderNativeId = 0;
  constexpr uint32_t kReceiverNativeId = 1;
  constexpr uint32_t kTokenNativeId = 2;

  ContractAddress sender_addr = make_contract_address(50);
  ContractAddress receiver_addr = make_contract_address(51);
  ContractAddress token_contract_addr = make_contract_address(52);

  auto const *cost_model = get_default_cost_model();
  auto str_hello = make_bytestring("hello");
  int128_t write_cost = cost_model->get_bytestring_cost(str_hello.size());
  ASSERT_GT(write_cost, 0);

  // The sender is the root invoked address, so its committed transaction also
  // consumes a replay nonce whose cost is charged at commit (after execution).
  // During execution the sender therefore still holds exactly this reserve
  // after transferring write_cost away; it is deducted at commit, ending at 0.
  auto replay_reserve = static_cast<uint128_t>(replay_cache_cost_charged(1));

  // Sender contract
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_sender;
  fns_sender.emplace(
      0,
      [token_contract_addr, receiver_addr, write_cost, replay_reserve](
          NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto native_token_client =
            ContractTraitClient<itc_erc20::ERC20>(token_contract_addr);

        uint128_t amount = write_cost;

        auto r_transfer =
            native_token_client.invoke_opt<u8"transfer">(ctx, receiver_addr,
                                                         make_amount(amount));

        auto transfer_result = check_result(r_transfer);
        if (!transfer_result.is_ok()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Invoke receiver contract to use the tokens
        auto r_receive = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          return ctx.syscall<SyscallNo::kInvoke>(receiver_addr,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        [[maybe_unused]] auto g_receive = GasAmount{0};

        if (!r_receive.has_value() ||
            r_receive.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto r_balance = native_token_client.invoke_opt<u8"get_balance">(ctx);
        auto balance_result = check_result(r_balance);
        if (!balance_result.is_ok()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto res = balance_result.ok();
        uint128_t balance_unsigned_int =
            uint128_t{std::get<0>(res)} << 64 | uint128_t{std::get<1>(res)};

        if (balance_unsigned_int != replay_reserve) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return {};
      });
  auto sender_contract =
      std::make_unique<NativeContract>(std::move(fns_sender));

  // Receiver contract
  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_receiver;
  fns_receiver.emplace(
      0, [str_hello](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key = make_storage_key("test");
        auto res = ctx.syscall<SyscallNo::kBytestringSet>(
            key, std::as_bytes(std::span(str_hello)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE()
              << "TransferAndUseStorageWithinTransaction: Failed to set "
                 "bytestring in receiver contract method 0";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto receiver_contract =
      std::make_unique<NativeContract>(std::move(fns_receiver));

  // Token contract
  auto token_contract = make_native_token_contract();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(kReceiverNativeId, std::move(receiver_contract));
  contract_map.emplace(kTokenNativeId, std::move(token_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy contracts
  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(sender_addr, sender_contract_xdr);

  auto receiver_contract_xdr = make_native_contract(kReceiverNativeId);
  write_contract(receiver_contract_xdr);
  write_deployment(receiver_addr, receiver_contract_xdr);

  auto token_contract_xdr = make_native_contract(kTokenNativeId);
  write_contract(token_contract_xdr);
  write_deployment(token_contract_addr, token_contract_xdr);

  // Grant initial tokens to sender. The sender is the root invoked address, so
  // its one committed tx also charges the replay-cache cost to it; grant that
  // on top of write_cost so the sender can both transfer write_cost away and
  // pay for the replay cache, ending at a balance of exactly 0.
  setup_grant_storage_token(sender_addr,
                            write_cost + replay_cache_cost_charged(1));
  setup_grant_storage_token(receiver_addr, 0);

  // Invoke the sender contract
  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = sender_addr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  // Check sender token balance
  auto sender_token_addrkey =
      AddressAndKey{.address = sender_addr,
                    .key = StorageKey(StorageKeyType::FreeNativeTokenBalance)};
  auto sender_token_item_opt =
      lookup_storage_item_from_db(sender_token_addrkey);
  ASSERT_TRUE(sender_token_item_opt.has_value());
  EXPECT_EQ(from_xdr(sender_token_item_opt.value().nnint()), 0);

  // Check receiver token balance
  auto receiver_token_addrkey =
      AddressAndKey{.address = receiver_addr,
                    .key = StorageKey(StorageKeyType::FreeNativeTokenBalance)};
  auto receiver_token_item_opt =
      lookup_storage_item_from_db(receiver_token_addrkey);
  ASSERT_TRUE(receiver_token_item_opt.has_value());
  EXPECT_EQ(from_xdr(receiver_token_item_opt.value().nnint()), 0);

  // Check receiver storage
  auto data_addrkey = make_contract_data_addrkey(receiver_addr, "test");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_TRUE(data_item_opt.has_value());
  ASSERT_EQ(data_item_opt.value().bytestring(), str_hello);

  // check that hog_db streamed out the results correctly
  if (GetParam() == TestDB::kHogDb) {
    auto hogs_transferred = get_hogs_transferred(1);
    ASSERT_EQ(hogs_transferred.size(), 1);
    // The sender's net balance delta for the block is the transfer out plus the
    // replay-cache cost newly locked against it as the root invoked address.
    ASSERT_EQ(hogs_transferred.at(sender_addr),
              -write_cost - replay_cache_cost_charged(1));
    ASSERT_FALSE(hogs_transferred.contains(receiver_addr));
  }
}

namespace {

constexpr ContractAddress kNNIntInterleavingWriter =
    NativeContractTestFramework::make_contract_address(0);
constexpr ContractAddress kNNIntInterleavingOrchestrator =
    NativeContractTestFramework::make_contract_address(1);

} // namespace

TEST_P(TryInvokeTest, NNIntAddSimpleInterleavings)
{

  constexpr static StorageKeyContractData kKey = make_storage_key("test");
  constexpr static int32_t kWriterNativeId = 0;
  constexpr static int32_t kOrchestratorNativeId = 1;

  static auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    NativeContractBuilder builder_writer;

    // perform a set_add_nnint
    builder_writer.add<NativeContractBuilder::kDefaultSerializationFormat>(
        0,
        [](NativeSyscallContext &ctx, int64_t set_amount,
           int64_t add_amount) -> InvokeStatus<void> {
          int128_t set_128 = set_amount;
          auto [r, _gas] = syscall_NonnegativeIntSetAdd_inner(
              ctx.handler(), kKey, &set_128, add_amount,
              std::to_underlying(StorageDeltaFlags::None));

          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });

    // perform a add_nnint
    builder_writer.add<NativeContractBuilder::kDefaultSerializationFormat>(
        1,
        [](NativeSyscallContext &ctx,
           int64_t add_amount) -> InvokeStatus<void> {
          auto [r, _gas] = syscall_NonnegativeIntSetAdd_inner(
              ctx.handler(), kKey, nullptr, add_amount,
              std::to_underlying(StorageDeltaFlags::None));
          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });

    builder_writer.add<NativeContractBuilder::kDefaultSerializationFormat>(
        3, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          // reset contract state
          int128_t set_value = 35;
          auto [r, _gas] = syscall_NonnegativeIntSetAdd_inner(
              ctx.handler(), kKey, &set_value, 0,
              std::to_underlying(StorageDeltaFlags::None));
          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });

    NativeContractBuilder builder_orchestrator;
    builder_orchestrator.add<
        NativeContractBuilder::kDefaultSerializationFormat>(
        0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          // perform an add_nnint on the writer, then a set_add_nnint with the
          // same set value on the writer in a try_invoke frame
          auto calldata0 =
              Serializer<NativeContractBuilder::kDefaultSerializationFormat>::
                  serialize<int64_t>(100)
                      .value();

          auto r0 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 1,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(kNNIntInterleavingWriter,
                                                   calldata0,
                                                   std::span<uint32_t>{},
                                                   inputs,
                                                   CopyableInvokeOutputs{});
          }();
          if (!r0.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          // tolerate try_invoke failure

          auto calldata1 =
              Serializer<NativeContractBuilder::kDefaultSerializationFormat>::
                  serialize<int64_t, int64_t>(100, -100)
                      .value();

          auto r1 = [&]() {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(kNNIntInterleavingWriter,
                                                   calldata1,
                                                   std::span<uint32_t>{},
                                                   inputs,
                                                   CopyableInvokeOutputs{});
          }();
          if (!r1.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          // tolerate try_invoke failure

          return {};
        });

    auto native_contract_writer = builder_writer.build();
    auto native_contract_orchestrator = builder_orchestrator.build();

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(kWriterNativeId, std::move(native_contract_writer));
    contract_map.emplace(kOrchestratorNativeId,
                         std::move(native_contract_orchestrator));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  static auto context = make_context();

  // Deploy writer and orchestrator; grant storage tokens on the writer only.
  auto writer_xdr = make_native_contract(kWriterNativeId);
  auto orchestrator_xdr = make_native_contract(kOrchestratorNativeId);
  write_contract(writer_xdr);
  write_contract(orchestrator_xdr);
  write_deployment(kNNIntInterleavingWriter, writer_xdr);
  write_deployment(kNNIntInterleavingOrchestrator, orchestrator_xdr);

  setup_grant_storage_token(kNNIntInterleavingWriter, 1'000'000'000);
  // The orchestrator is also a root invoked address (it commits set_add_nnint
  // transactions), so it must afford the replay-cache cost per commit.
  setup_grant_storage_token(kNNIntInterleavingOrchestrator, 1'000'000'000);

  // write an initial int128 value
  StorageKey wrapped_key(StorageKeyType::ContractData);
  wrapped_key.contract_data_key() = kKey;

  // too much state here to parallelize the experiment
  model::WorkQueue work_queue;

  while (!work_queue.done()) {
    model::RunnableActionSet actions(work_queue);

    actions.add_action(
        [](model::RunnableActionSet &set, auto self) -> model::Async {
          co_await set.bg();

          auto handler_tx = SignedTransaction{
              .tx =
                  Transaction{.invocation =
                                  Invocation{.invoked_address =
                                                 kNNIntInterleavingOrchestrator,
                                             .entrypoint =
                                                 entrypoint_from_method_id(0),
                                             .gas_limit = UINT64_MAX,
                                             .calldata = {}},
                              .uniquifier = self->make_new_uniquifier()}};
          auto handler =
              self->make_new_system_call_handler(handler_tx, *context);
          auto res = handler.root_invoke();
          if (!res.is_root_success()) {
            panic("root invoke 0 failed");
          }
          co_await set.bg();
          auto commit_res = handler.try_commit_transaction();
          if (commit_res != ExecResult::ExecResultOK &&
              commit_res != ExecResult::ExecResultTxNonDeterministicError) {
            panic("try_commit_transaction error: {}", x23::show(commit_res));
          }
          co_return;
        },
        this);

    actions.add_action(
        [](model::RunnableActionSet &set, auto self) -> model::Async {
          co_await set.bg();

          // If this one precedes the set_add_nnint in the orchestrator, then
          // that will succeed.
          auto calldata =
              Serializer<NativeContractBuilder::kDefaultSerializationFormat>::
                  serialize<int64_t, int64_t>(100, 100)
                      .value();
          auto handler_tx = SignedTransaction{
              .tx = Transaction{
                  .invocation =
                      Invocation{.invoked_address = kNNIntInterleavingWriter,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = calldata},
                  .uniquifier = self->make_new_uniquifier()}};
          auto handler =
              self->make_new_system_call_handler(handler_tx, *context);
          auto res = handler.root_invoke();
          co_await set.bg();
          if (!res.is_root_success()) {
            co_return;
          }
          auto commit_res = handler.try_commit_transaction();
          if (commit_res != ExecResult::ExecResultOK &&
              commit_res != ExecResult::ExecResultTxNonDeterministicError) {
            panic("try_commit_transaction error: {}", x23::show(commit_res));
          }
          co_return;
        },
        this);

    {
      auto handler_tx = SignedTransaction{
          .tx = Transaction{
              .invocation =
                  Invocation{.invoked_address = kNNIntInterleavingWriter,
                             .entrypoint = entrypoint_from_method_id(3),
                             .gas_limit = UINT64_MAX,
                             .calldata = {}},
              .uniquifier = make_new_uniquifier()}};
      auto handler = make_new_system_call_handler(handler_tx, *context);

      auto res = handler.root_invoke();
      ASSERT_TRUE(res.is_root_success());
      auto commit_res = handler.try_commit_transaction();
      ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
      ASSERT_TRUE(
          handler.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = handler_tx})());
    }

    advance_block();

    ASSERT_EQ(actions.run(), model::ActionResult::kOk);

    advance_block();

    auto res = lookup_storage_item_from_db(
        AddressAndKey{.address = kNNIntInterleavingWriter, .key = wrapped_key});
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(res.value().type(), StorageItemType::kItemTypeNonnegativeInt);
    int128_t value = from_xdr(res.value().nnint());

    ASSERT_TRUE(value == 100 || value == 0);

    work_queue.advance_cursor();
  }
}

TEST_P(TryInvokeTest, InvokeErrorHandling)
{
  // This test is a guard against an unchecked error condition in the
  // invoke syscall handler (fixing an earlier bug, PR #200).
  //
  // There are many ways that an invocation can fail.  This test makes sure
  // those errors are passed back to the caller correctly.

  // This test creates two native contracts.  The first has two methods.
  // The first method returns a deterministic error and consumes 100 gas.
  // The second method returns success.
  // The second contract has one method.
  // These are both deployed to separate addresses.
  // This second contract's method performs the following actions:
  // 1. try_invoke on a third address, which has no contract deployed there.
  //    The try_invoke should fail, and the error should be checked as the
  //    appropriate error code.
  // 2. try_invoke on the first contract, but with a method number that does not
  // exist.
  //    The try_invoke should fail, and the error should be checked as the
  //    appropriate error code.
  // 3. try_invoke on the first contract, with the correct method number, but
  // with a gas limit of 50.
  //    The try_invoke should fail, and the error should be checked as the
  //    appropriate error code.
  // 4. try_invoke on the first contract, with the correct method number, but
  // with a gas limit of 1000.
  //    The try_invoke should succeed, and the error should be checked as the
  //    appropriate error code.
  // 5. try_invoke on the first contract, on the method that returns success.
  // Should succeed.

  constexpr int32_t kErrorContractId = 0;
  constexpr int32_t kCallerContractId = 1;
  ContractAddress error_contract_addr = make_contract_address(10);
  ContractAddress caller_contract_addr = make_contract_address(11);
  ContractAddress missing_contract_addr = make_contract_address(12);

  // Contract 0: always errors, consumes 100 gas
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_err;
  fns_err.emplace(0,
                  [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                    if (!ctx.consume_gas(100)) {
                      return InvokeError{InvokeErrorCode::OutOfGas};
                    }
                    return InvokeError{InvokeErrorCode::Deterministic};
                  });
  fns_err.emplace(1, [](NativeSyscallContext &) noexcept -> InvokeStatus<void> {
    return {};
  });
  auto error_contract = std::make_unique<NativeContract>(std::move(fns_err));

  // Contract 1: performs the four try_invoke error cases
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [=](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        Calldata calldata = {};
        // 1. try_invoke on a missing contract
        {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = 1000,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          auto res = ctx.syscall<SyscallNo::kInvoke>(
              missing_contract_addr, std::as_bytes(std::span(calldata)),
              std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});

          if (!res.has_value() ||
              res.value() !=
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        // 2. try_invoke on error_contract, but with a method that does not
        // exist
        {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 999,
              .gas_limit = 1000,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          CopyableInvokeOutputs outputs{};
          auto res = ctx.syscall<SyscallNo::kInvoke>(
              error_contract_addr, std::as_bytes(std::span(calldata)),
              std::span<uint32_t>{}, inputs, outputs);
          // The child frame ran and faulted (missing method), so the syscall
          // returns kSyscallErrorCodeNone with frame_status Fault.
          if (!res.has_value() ||
              res.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
              outputs.frame_status != InvocationFrameStatusCode::Fault) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        // 3. try_invoke on error_contract, correct method, but with gas
        // limit 50 (not enough)
        {
          auto gas_before = ctx.total_gas_consumed();
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = 50,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          CopyableInvokeOutputs outputs{};
          auto res = ctx.syscall<SyscallNo::kInvoke>(
              error_contract_addr, std::as_bytes(std::span(calldata)),
              std::span<uint32_t>{}, inputs, outputs);
          auto gas_used = ctx.total_gas_consumed() - gas_before;
          // The child frame ran out of gas (handleable fault), with 50 gas
          // consumed: the syscall returns kSyscallErrorCodeNone with
          // frame_status Fault.
          if (!res.has_value() ||
              res.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
              outputs.frame_status != InvocationFrameStatusCode::Fault) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (gas_used != 50) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        // 4. try_invoke on error_contract, correct method, with gas limit
        // 1000 (enough)
        {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = 1000,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          CopyableInvokeOutputs outputs{};
          auto res = ctx.syscall<SyscallNo::kInvoke>(
              error_contract_addr, std::as_bytes(std::span(calldata)),
              std::span<uint32_t>{}, inputs, outputs);
          // The child frame ran and faulted (deterministic error), so the
          // syscall returns kSyscallErrorCodeNone with frame_status Fault.
          if (!res.has_value() ||
              res.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
              outputs.frame_status != InvocationFrameStatusCode::Fault) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        // 5. try_invoke on error_contract, valid method, with gas limit
        // 1000 (enough)
        {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 1,
              .gas_limit = 1000,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          auto res = ctx.syscall<SyscallNo::kInvoke>(
              error_contract_addr, std::as_bytes(std::span(calldata)),
              std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
          if (!res.has_value() ||
              res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }

        return {};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kErrorContractId, std::move(error_contract));
  contract_map.emplace(kCallerContractId, std::move(caller_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy contracts
  auto error_contract_xdr = make_native_contract(kErrorContractId);
  write_contract(error_contract_xdr);
  write_deployment(error_contract_addr, error_contract_xdr);

  auto caller_contract_xdr = make_native_contract(kCallerContractId);
  write_contract(caller_contract_xdr);
  write_deployment(caller_contract_addr, caller_contract_xdr);

  // Grant tokens to both contracts
  setup_grant_storage_token(error_contract_addr, 1'000'000'000);
  setup_grant_storage_token(caller_contract_addr, 1'000'000'000);

  // Now invoke the caller contract
  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = caller_contract_addr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
}

TEST_P(TryInvokeTest, ManualRevertUndoesTryInvokeLocks)
{
  // Assert that if a try_invoke frame successfully acquires locks but then its
  // parent frame does a manual revert, then the locks are released. This test
  // uses three contracts:
  // Entry: try_invoke middle:0.
  // Middle: try_invoke writer:0, then manually revert.
  // Writer:0 writes "hello"; writer:1 asserts the key has no bytestring (used
  // from entry after the middle frame returns). This test invokes entry:0.
  // Afterwards, it asserts that the bytestring was not written on the writer.

  static constexpr ContractAddress kAddressWriter = make_contract_address(47);
  static constexpr ContractAddress kAddressMiddle = make_contract_address(48);
  static constexpr ContractAddress kAddressEntry = make_contract_address(49);

  // Bytestring to write
  auto str_hello = make_bytestring("hello");
  auto key = make_storage_key("test");

  // Writer: 0 — write "hello"; 1 — verify key has no bytestring (for entry's
  // post-revert check on writer storage).
  // Middle: 0 — try_invoke writer 0, then manually revert.
  // Entry: 0 — try_invoke middle 0; try_invoke writer 1; return-buffer checks.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_w;
  fns_w.emplace(0,
                [str_hello, key](
                    NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto [res, _gas] = syscall_BytestringSet_inner(
                      ctx.handler(), key, std::as_bytes(std::span(str_hello)),
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!res.has_value() ||
                      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    ADD_FAILURE()
                        << "ManualRevertUndoesTryInvokeLocks: Failed to "
                           "set bytestring in writer method 0";
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  return {};
                });
  fns_w.emplace(1,
                [key](
                    NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto res = ctx.handler().get_bytestring(key);
                  if (!res.has_value()) {
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  if (res.value().has_value()) {
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  return {};
                });
  auto native_contract_writer =
      std::make_unique<NativeContract>(std::move(fns_w));
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_m;
  fns_m.emplace(0,
                [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  // Try invoke writer method 0
                  auto r2 = [&]() {
                    CopyableInvokeInputs inputs{
                        .entrypoint_id = 0,
                        .gas_limit = UINT64_MAX,
                        .flags = std::to_underlying(InvokeFlag::TryInvoke),
                    };
                    return ctx.syscall<SyscallNo::kInvoke>(
                        kAddressWriter, std::span<const std::byte>{},
                        std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
                  }();
                  if (!r2.has_value()) {
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  // Writer method 0 should succeed in the try_invoke frame
                  if (r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  // Now manually revert this frame, which should undo the locks
                  // acquired by writer method 0
                  ctx.handler().set_return_buffer(ReturnBuffer{}, true);
                  return {};
                });
  auto native_contract_middle =
      std::make_unique<NativeContract>(std::move(fns_m));
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_e;
  fns_e.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Try invoke middle method 0
        CopyableInvokeOutputs r1_outputs{};
        auto r1 = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressMiddle,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 r1_outputs);
        }();
        if (!r1.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // The middle frame ran, so the syscall returns kSyscallErrorCodeNone;
        // its manual revert is reported via frame_status Abort.
        if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            r1_outputs.frame_status != InvocationFrameStatusCode::Abort) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Writer storage should not retain the write; verify via writer
        // method 1 (same contract that would have held the key).
        auto r_w = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 1,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressWriter,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!r_w.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (r_w.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // assert that we can actually get a ReturnBuffer back
        auto return_buf = ctx.handler().get_return_buffer();
        if (!return_buf.has_value() || !return_buf.value().has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        assert(**return_buf);
        if (!return_buf.value().value()->empty()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return {};
      });
  auto native_contract_entry =
      std::make_unique<NativeContract>(std::move(fns_e));
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_writer));
  contract_map.emplace(1, std::move(native_contract_middle));
  contract_map.emplace(2, std::move(native_contract_entry));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // Deploy contracts; grant storage tokens on the writer only
  auto writer_xdr = make_native_contract(0);
  auto middle_xdr = make_native_contract(1);
  auto entry_xdr = make_native_contract(2);
  write_contract(writer_xdr);
  write_contract(middle_xdr);
  write_contract(entry_xdr);
  write_deployment(kAddressWriter, writer_xdr);
  write_deployment(kAddressMiddle, middle_xdr);
  write_deployment(kAddressEntry, entry_xdr);
  setup_grant_storage_token(kAddressWriter, 1'000'000'000);
  // kAddressEntry is the root invoked address, so its one committed tx charges
  // the replay-cache cost to it; grant exactly that so the commit can pay it.
  setup_grant_storage_token(kAddressEntry, replay_cache_cost_charged(1));

  // Invoke entry method 0
  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressEntry,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  // Check that the bytestring was NOT written on the writer (locks were
  // released due to manual revert)
  auto data_addrkey = make_contract_data_addrkey(kAddressWriter, "test");
  auto data_item_opt = lookup_storage_item_from_db(data_addrkey);
  ASSERT_FALSE(data_item_opt.has_value());
}
// NOLINTEND(bugprone-unchecked-optional-access)

TEST_P(TryInvokeTest, RCQSelfConsumeAfterPriorTryInvokeSucceeds)
{
  if (GetParam() != TestDB::kHogDb) {
    GTEST_SKIP()
        << "HogDb-only: this exercises subtransaction narrow() behavior";
  }

  static constexpr ContractAddress kAddressTarget = make_contract_address(72);
  static constexpr ContractAddress kAddressOrchestrator =
      make_contract_address(73);

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_t;
    fns_t.emplace(0,
                  [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                    auto key = make_storage_key("rcq-self-consume");
                    uint8_t entry = 0xA5;
                    auto res = ctx.handler().retrocausal_queue_insert(
                        key, std::as_bytes(std::span(&entry, 1)));
                    if (!res.has_value()) {
                      return res.error().to_invoke_error();
                    }
                    if (!res.value().has_value()) {
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }
                    return {};
                  });
    fns_t.emplace(1,
                  [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                    auto key = make_storage_key("rcq-self-consume");
                    auto res =
                        ctx.handler().retrocausal_queue_consume(key, false);
                    if (!res.has_value()) {
                      return res.error().to_invoke_error();
                    }
                    if (!res.value().has_value()) {
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }

                    auto const &entry = res.value().value();
                    if (entry.size() != 1 || entry[0] != 0xA5) {
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }
                    return {};
                  });
    auto native_contract_target =
        std::make_unique<NativeContract>(std::move(fns_t));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_o;
    fns_o.emplace(0,
                  [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                    auto invoke_target = [&](Runtime::MethodID method) {
                      CopyableInvokeInputs inputs{
                          .entrypoint_id = method,
                          .gas_limit = UINT64_MAX,
                          .flags = std::to_underlying(InvokeFlag::TryInvoke),
                      };
                      return ctx.syscall<SyscallNo::kInvoke>(
                          kAddressTarget, std::span<const std::byte>{},
                          std::span<uint32_t>{}, inputs,
                          CopyableInvokeOutputs{});
                    };

                    auto r0 = invoke_target(0);
                    if (!r0.has_value() ||
                        r0.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }

                    auto r1 = invoke_target(1);
                    if (!r1.has_value()) {
                      if (r1.error().code() == HostFnErrorCode::Unrecoverable) {
                        return InvokeError{InvokeErrorCode::Unrecoverable};
                      }
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }
                    if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }
                    return {};
                  });
    auto native_contract_orchestrator =
        std::make_unique<NativeContract>(std::move(fns_o));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_target));
    contract_map.emplace(1, std::move(native_contract_orchestrator));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {kAddressTarget, kAddressOrchestrator};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressOrchestrator,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success())
      << "RCQ self-consume across TryInvoke frames must not be Unrecoverable";
}

TEST_P(TryInvokeTest, RCQMultipleSelfConsumeAndReinsert)
{
  if (GetParam() != TestDB::kHogDb) {
    GTEST_SKIP()
        << "HogDb-only: this exercises subtransaction narrow() behavior";
  }

  static constexpr ContractAddress kAddressTarget = make_contract_address(74);
  static constexpr ContractAddress kAddressOrchestrator =
      make_contract_address(75);

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_t;
    fns_t.emplace(0,
                  [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                    auto key = make_storage_key("rcq-multi");
                    uint8_t entry = 0xBB;
                    auto res = ctx.handler().retrocausal_queue_insert(
                        key, std::as_bytes(std::span(&entry, 1)));
                    if (!res.has_value()) {
                      return res.error().to_invoke_error();
                    }
                    if (!res.value().has_value()) {
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }
                    return {};
                  });
    fns_t.emplace(1,
                  [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                    auto key = make_storage_key("rcq-multi");
                    auto res =
                        ctx.handler().retrocausal_queue_consume(key, false);
                    if (!res.has_value()) {
                      return res.error().to_invoke_error();
                    }
                    if (!res.value().has_value()) {
                      return InvokeError{InvokeErrorCode::Deterministic};
                    }
                    return {};
                  });
    auto native_contract_target =
        std::make_unique<NativeContract>(std::move(fns_t));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_o;
    fns_o.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          auto invoke_target = [&](Runtime::MethodID method) {
            CopyableInvokeInputs inputs{
                .entrypoint_id = method,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            return ctx.syscall<SyscallNo::kInvoke>(kAddressTarget,
                                                   std::span<const std::byte>{},
                                                   std::span<uint32_t>{},
                                                   inputs,
                                                   CopyableInvokeOutputs{});
          };

          // insert -> consume -> insert (grow, shrink, grow)
          for (auto method : {0, 1, 0}) {
            auto r = invoke_target(method);
            if (!r.has_value()) {
              return r.error().code() == HostFnErrorCode::Unrecoverable
                         ? InvokeError{InvokeErrorCode::Unrecoverable}
                         : InvokeError{InvokeErrorCode::Deterministic};
            }
            if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
              return InvokeError{InvokeErrorCode::Deterministic};
            }
          }
          return {};
        });
    auto native_contract_orchestrator =
        std::make_unique<NativeContract>(std::move(fns_o));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_target));
    contract_map.emplace(1, std::move(native_contract_orchestrator));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {kAddressTarget, kAddressOrchestrator};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressOrchestrator,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success())
      << "insert-consume-insert via TryInvoke must succeed";
}

TEST_P(TryInvokeTest, TryInvokeHitsSavepointDepthLimit)
{
  // One savepoint slot is reserved for the root commit frame, so the
  // effective child TryInvoke cap is kMaxSavepointDepth - 1.
  static constexpr int kChildCap = kMaxSavepointDepth - 1;

  static constexpr ContractAddress kAddressNoop = make_contract_address(0);
  static constexpr ContractAddress kAddressLooper = make_contract_address(1);

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_noop;
    fns_noop.emplace(0,
                     [](NativeSyscallContext &) noexcept -> InvokeStatus<void> {
                       return {};
                     });
    auto noop = std::make_unique<NativeContract>(std::move(fns_noop));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_looper;
    fns_looper.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          int successes = 0;
          for (int i = 0; i < kChildCap + 1; i++) {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            CopyableInvokeOutputs outputs{};
            auto r =
                ctx.syscall<SyscallNo::kInvoke>(kAddressNoop,
                                                std::span<const std::byte>{},
                                                std::span<uint32_t>{}, inputs,
                                                outputs);
            if (!r.has_value()) {
              return InvokeError{InvokeErrorCode::Deterministic};
            }
            // Every child runs (the depth-cap overflow is converted to a
            // deterministic failure, so the syscall still returns
            // kSyscallErrorCodeNone); a child committed only when its
            // frame_status is Return.
            if (r.value() == SyscallErrorCode::kSyscallErrorCodeNone &&
                outputs.frame_status == InvocationFrameStatusCode::Return) {
              successes++;
            }
          }
          if (successes != kChildCap) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    auto looper = std::make_unique<NativeContract>(std::move(fns_looper));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(noop));
    contract_map.emplace(1, std::move(looper));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {kAddressNoop, kAddressLooper};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressLooper,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
}

TEST_P(TryInvokeTest, RootCommitSucceedsAfterMaxChildTryInvokeDepth)
{
  // Regression test: after exhausting the child TryInvoke cap, root commit
  // must still succeed because one savepoint slot is reserved for it.
  if (GetParam() != TestDB::kHogDb) {
    GTEST_SKIP() << "HogDb savepoint cap is required for this regression";
  }

  static constexpr int kChildCap = kMaxSavepointDepth - 1;

  static constexpr ContractAddress kAddressNoop = make_contract_address(0);
  static constexpr ContractAddress kAddressLooper = make_contract_address(1);

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_noop;
    fns_noop.emplace(0,
                     [](NativeSyscallContext &) noexcept -> InvokeStatus<void> {
                       return {};
                     });
    auto noop = std::make_unique<NativeContract>(std::move(fns_noop));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_looper;
    fns_looper.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          int successes = 0;
          for (int i = 0; i < kChildCap + 1; i++) {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::TryInvoke),
            };
            CopyableInvokeOutputs outputs{};
            auto r =
                ctx.syscall<SyscallNo::kInvoke>(kAddressNoop,
                                                std::span<const std::byte>{},
                                                std::span<uint32_t>{}, inputs,
                                                outputs);
            if (!r.has_value()) {
              return InvokeError{InvokeErrorCode::Deterministic};
            }
            // Every child runs (the depth-cap overflow is converted to a
            // deterministic failure, so the syscall still returns
            // kSyscallErrorCodeNone); a child committed only when its
            // frame_status is Return.
            if (r.value() == SyscallErrorCode::kSyscallErrorCodeNone &&
                outputs.frame_status == InvocationFrameStatusCode::Return) {
              successes++;
            }
          }
          if (successes != kChildCap) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    auto looper = std::make_unique<NativeContract>(std::move(fns_looper));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(noop));
    contract_map.emplace(1, std::move(looper));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {kAddressNoop, kAddressLooper};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kAddressLooper,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  EXPECT_EQ(commit_res, ExecResult::ExecResultOK);
}

INSTANTIATE_TEST_SUITE_P(TestDBs, TryInvokeTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
