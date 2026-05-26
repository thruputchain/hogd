#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "built_contracts/contract_loader.h"
#include "built_contracts/fibonacci_call.h"
#include "built_contracts/no_syscall_test.h"
#include "built_contracts/sha256_wrapper.h"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "invoke_flag.hh"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class TracingTest : public ::testing::Test, public NativeContractTestFramework {
public:
  TracingTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

protected:
  static std::unique_ptr<RuntimeContext> make_context()
  {
    return make_hogexec_wasm_context(RuntimeEngine::kWasmi);
  };

  std::unique_ptr<RuntimeContext> runtime_context_;

  void SetUp() override { runtime_context_ = make_hogexec_context(); }
};

TEST_F(TracingTest, Sha256Repeated)
{
  auto contract = hogd::load_contract("sha256_wrapper.wasm");
  auto contract_addr = make_contract_address(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto client =
      ContractTraitClient<sha256_wrapper::Sha256Wrapper>(contract_addr);

  uint32_t n = 5;
  x23::Vec<uint8_t, 0xffffffff> data = {1, 2, 3, 4, 5};

  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client.make_invocation<"sha256_repeated">(10000, n, data)},
      *runtime_context_);
  handler.enable_tracing();
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  auto const *trace = handler.get_trace();

  ASSERT_EQ(trace->invocation.gas_limit, 10000);
  ASSERT_EQ(trace->system_calls.size(),
            n + 2); // +2 for get_calldata and return
  uint64_t gas_remaining = 10000;

  ASSERT_EQ(trace->system_calls[0].body.syscall_number(),
            SyscallNo::kGetCalldata);

  for (size_t i = 1; i < n + 1; i++) {
    ASSERT_LT(trace->system_calls[i].gas_remaining_at_start, gas_remaining);
    ASSERT_LT(trace->system_calls[i].gas_remaining_at_end,
              trace->system_calls[i].gas_remaining_at_start);
    gas_remaining = trace->system_calls[i].gas_remaining_at_end;
    ASSERT_EQ(trace->system_calls[i].body.syscall_number(), SyscallNo::kSHA256);
    auto const &trace_call = trace->system_calls[i].body.sha256();
    ASSERT_EQ(trace->system_calls[i].status.syscall_return_code(),
              SyscallErrorCode::kSyscallErrorCodeNone);
    ASSERT_EQ(trace_call.inputs.input, data);
    ASSERT_NE(trace_call.outputs, nullptr);
    ASSERT_EQ(trace_call.outputs->hash, sha256(data));
    ASSERT_EQ(trace->system_calls[i].status.error(),
              HostFnErrorCode::NoneOrRecoverable);
    ASSERT_EQ(trace->system_calls[i].status.syscall_return_code(),
              SyscallErrorCode::kSyscallErrorCodeNone);
  }

  ASSERT_EQ(trace->system_calls[n + 1].body.syscall_number(),
            SyscallNo::kReturn);
}

TEST_F(TracingTest, FibonacciNestedInvocation)
{
  auto fibonacci_contract = hogd::load_contract("fibonacci.wasm");
  auto fibonacci_caller_contract = hogd::load_contract("fibonacci_call.wasm");

  auto fibonacci_addr = make_contract_address(0);
  auto fibonacci_caller_addr = make_contract_address(1);

  write_contract(fibonacci_contract);
  write_contract(fibonacci_caller_contract);
  write_deployment(fibonacci_addr, fibonacci_contract);
  write_deployment(fibonacci_caller_addr, fibonacci_caller_contract);

  setup_grant_storage_token(fibonacci_addr, 1'000'000'000);
  setup_grant_storage_token(fibonacci_caller_addr, 1'000'000'000);

  auto caller_client = ContractTraitClient<fibonacci_call::FibonacciCaller>(
      fibonacci_caller_addr);

  // Initialize fibonacci_call with the fibonacci contract's address
  {
    auto handler = make_new_system_call_handler(
        Transaction{.invocation = caller_client.make_invocation<"initialize">(
                        UINT64_MAX, fibonacci_addr)},
        *runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    advance_block();
  }

  // Call call_fibonacci(5, gas_limit) with tracing enabled
  uint64_t gas_limit = 1'000'000;
  auto handler = make_new_system_call_handler(
      Transaction{.invocation = caller_client.make_invocation<"call_fibonacci">(
                      gas_limit, uint32_t{5}, gas_limit)},
      *runtime_context_);
  handler.enable_tracing();
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  auto const *trace = handler.get_trace();
  ASSERT_NE(trace, nullptr);

  // Root frame should be the fibonacci_call contract
  ASSERT_EQ(trace->invocation.invoked_address, fibonacci_caller_addr);

  // Root frame should have terminated with Return
  ASSERT_EQ(trace->status.status_code(), InvocationFrameStatusCode::Return);

  // Find the Invoke syscall in the root frame
  const SyscallTrace *invoke_syscall = nullptr;
  for (auto const &sc : trace->system_calls) {
    if (sc.body.syscall_number() == SyscallNo::kInvoke) {
      invoke_syscall = &sc;
      break;
    }
  }
  ASSERT_NE(invoke_syscall, nullptr);

  // The Invoke syscall should have outputs with a child_trace
  auto const &invoke_trace = invoke_syscall->body.invoke();
  ASSERT_NE(invoke_trace.outputs, nullptr);
  ASSERT_NE(invoke_trace.outputs->child_trace, nullptr);

  auto const &child_trace = *invoke_trace.outputs->child_trace;

  // Child frame should be the fibonacci contract
  ASSERT_EQ(child_trace.invocation.invoked_address, fibonacci_addr);

  // Child frame should have terminated with Return
  ASSERT_EQ(child_trace.status.status_code(),
            InvocationFrameStatusCode::Return);

  // Child frame should have its own syscalls (at least GetCalldata + Return)
  ASSERT_GE(child_trace.system_calls.size(), 2U);
  ASSERT_EQ(child_trace.system_calls[0].body.syscall_number(),
            SyscallNo::kGetCalldata);
  ASSERT_EQ(child_trace.system_calls[child_trace.system_calls.size() - 1]
                .body.syscall_number(),
            SyscallNo::kReturn);

  // Gas accounting: child frame gas_limit == parent gas at invoke point
  // (some contracts might set <, but this contract delegates the full amount)
  ASSERT_EQ(child_trace.invocation.gas_limit,
            invoke_syscall->gas_remaining_at_start);
}

TEST_F(TracingTest, NoSyscallTest)
{
  auto contract_addr = make_contract_address(0);
  auto client =
      ContractTraitClient<no_syscall_test::NoSyscallTest>(contract_addr);
  auto contract = load_contract("no_syscall_test.wasm");
  write_contract(contract);
  write_deployment(contract_addr, contract);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client.make_invocation<"run_test">(10000, uint64_t{12345})},
      *runtime_context_);
  handler.enable_tracing();
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  auto const *trace = handler.get_trace();

  ASSERT_EQ(trace->invocation.gas_limit, 10000);
  ASSERT_EQ(trace->system_calls.size(),
            3); // +2 for get_calldata and return

  ASSERT_EQ(trace->system_calls[0].body.syscall_number(),
            SyscallNo::kGetCalldata);

  ASSERT_EQ(trace->system_calls[1].body.syscall_number(),
            static_cast<SyscallNo>(12345));
  ASSERT_EQ(trace->system_calls[1].status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeNoSyscall);

  ASSERT_EQ(trace->system_calls[2].body.syscall_number(), SyscallNo::kReturn);
}

TEST_F(TracingTest, NoSyscallStatsDoNotCorruptCounters)
{
  auto contract_addr = make_contract_address(0);
  auto client =
      ContractTraitClient<no_syscall_test::NoSyscallTest>(contract_addr);
  auto contract = load_contract("no_syscall_test.wasm");
  write_contract(contract);
  write_deployment(contract_addr, contract);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      Transaction{.invocation = client.make_invocation<"run_test">(
                      10000, static_cast<uint64_t>(kSyscallCount))},
      *runtime_context_);
  handler.enable_transaction_stats();
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  auto *stats = handler.get_transaction_stats();
  ASSERT_NE(stats, nullptr);
  EXPECT_EQ(stats->invalid_syscall_count, 1);
  EXPECT_EQ(stats->syscall_fault_count, 0);
  EXPECT_EQ(stats->syscall_counts[std::to_underlying(SyscallNo::kGetCalldata)],
            1);
  EXPECT_EQ(stats->syscall_counts[std::to_underlying(SyscallNo::kReturn)], 1);
}

TEST_F(TracingTest, NestedInvokeDeploymentNotFound)
{
  auto contract_0_addr = make_contract_address(0);
  auto target_addr = make_contract_address(1); // No deployment

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    RuntimeMemoryLength output_return_buffer_len = 0,
                        output_returned_auth_token_len = 0;
    Calldata calldata{};
    auto r = ctx.syscall<SyscallNo::kInvoke>(target_addr, 0, UINT64_MAX,
                                             std::as_bytes(std::span(calldata)),
                                             0, std::span<uint32_t>{},
                                             output_return_buffer_len,
                                             output_returned_auth_token_len);

    if (!r.has_value() ||
        r.value() != SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
      ADD_FAILURE() << "Expected kSyscallErrorCodeInvalidSyscallInput";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0_xdr = make_native_contract(0);
  write_contract(contract_0_xdr);
  write_deployment(contract_0_addr, contract_0_xdr);
  setup_grant_storage_token(contract_0_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = contract_0_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}}},
      *runtime_context);
  handler.enable_tracing();
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  auto const *trace = handler.get_trace();
  ASSERT_NE(trace, nullptr);

  // Root frame should be contract 0
  ASSERT_EQ(trace->invocation.invoked_address, contract_0_addr);

  // Root frame should have terminated with Return (caller handled the error)
  ASSERT_EQ(trace->status.status_code(), InvocationFrameStatusCode::Return);

  // Find the Invoke syscall in the root frame
  const SyscallTrace *invoke_syscall = nullptr;
  for (auto const &sc : trace->system_calls) {
    if (sc.body.syscall_number() == SyscallNo::kInvoke) {
      invoke_syscall = &sc;
      break;
    }
  }
  ASSERT_NE(invoke_syscall, nullptr);

  // Invoke should have returned kSyscallErrorCodeInvalidSyscallInput
  ASSERT_EQ(invoke_syscall->status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

  // No child trace — invoke returned before start_frame
  auto const &invoke_trace = invoke_syscall->body.invoke();
  ASSERT_EQ(invoke_trace.outputs, nullptr);
}

TEST_F(TracingTest, NestedInvokeDeterministicError)
{
  auto caller_addr = make_contract_address(0);
  auto callee_addr = make_contract_address(1);

  // Callee contract: method 0 returns a deterministic error
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_callee;
  fns_callee.emplace(
      0, [](NativeSyscallContext & /*unused*/) noexcept -> InvokeStatus<void> {
        return InvokeError{InvokeErrorCode::Deterministic};
      });
  auto callee_contract =
      std::make_unique<NativeContract>(std::move(fns_callee));

  // Caller contract: method 0 invokes callee method 0
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        RuntimeMemoryLength output_return_buffer_len = 0,
                            output_returned_auth_token_len = 0;
        Calldata calldata{};
        auto r =
            ctx.syscall<SyscallNo::kInvoke>(callee_addr, 0, UINT64_MAX,
                                            std::as_bytes(std::span(calldata)),
                                            0, std::span<uint32_t>{},
                                            output_return_buffer_len,
                                            output_returned_auth_token_len);

        if (!r.has_value() ||
            r.value() != SyscallErrorCode::kSyscallErrorCodeDeterministicWasm) {
          ADD_FAILURE() << "Expected kSyscallErrorCodeDeterministicWasm";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(callee_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  auto callee_xdr = make_native_contract(1);
  write_contract(caller_xdr);
  write_contract(callee_xdr);
  write_deployment(caller_addr, caller_xdr);
  write_deployment(callee_addr, callee_xdr);
  setup_grant_storage_token(caller_addr, 1'000'000'000);
  setup_grant_storage_token(callee_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = caller_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}}},
      *runtime_context);
  handler.enable_tracing();
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  auto const *trace = handler.get_trace();
  ASSERT_NE(trace, nullptr);

  // Root frame should have terminated with Return (caller handled error)
  ASSERT_EQ(trace->status.status_code(), InvocationFrameStatusCode::Return);

  // Find the Invoke syscall in the root frame
  const SyscallTrace *invoke_syscall = nullptr;
  for (auto const &sc : trace->system_calls) {
    if (sc.body.syscall_number() == SyscallNo::kInvoke) {
      invoke_syscall = &sc;
      break;
    }
  }
  ASSERT_NE(invoke_syscall, nullptr);

  // Invoke should report kSyscallErrorCodeDeterministicWasm
  ASSERT_EQ(invoke_syscall->status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeDeterministicWasm);

  // Should have a child trace
  auto const &invoke_trace = invoke_syscall->body.invoke();
  ASSERT_NE(invoke_trace.outputs, nullptr);
  ASSERT_NE(invoke_trace.outputs->child_trace, nullptr);

  auto const &child_trace = *invoke_trace.outputs->child_trace;

  // Child frame should be the callee contract
  ASSERT_EQ(child_trace.invocation.invoked_address, callee_addr);

  ASSERT_EQ(child_trace.status.status_code(), InvocationFrameStatusCode::Fault);
}

TEST_F(TracingTest, TryInvokeLockAcquireFailure)
{
  // Two contracts (no self-reentrance): writer stores calldata under "test"
  // with an exclusive lock; caller try_invokes writer:0.  Uncommitted tx1
  // leaves the lock held so tx2's try_invoke fails lock acquisition.

  ContractAddress addr_writer = make_contract_address(0);
  ContractAddress addr_caller = make_contract_address(1);

  auto make_context = [&]() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_writer;
    fns_writer.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          auto const &calldata = ctx.handler().get_calldata();
          auto key = make_storage_key("test");
          auto [res, _gas] = syscall_BytestringSet_inner(
              ctx.handler(), key, std::as_bytes(std::span(calldata)),
              std::to_underlying(StorageDeltaFlags::ExclusiveLock));
          if (!res.has_value() ||
              res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to set bytestring";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    auto contract_writer =
        std::make_unique<NativeContract>(std::move(fns_writer));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_caller;
    fns_caller.emplace(
        0,
        [addr_writer](
            NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          Calldata args = {'t', 'e', 's', 't'};
          RuntimeMemoryLength output_return_buffer_len = 0,
                              output_returned_auth_token_len = 0;
          [[maybe_unused]] auto r = ctx.syscall<SyscallNo::kInvoke>(
              addr_writer, 0, UINT64_MAX, std::as_bytes(std::span(args)),
              std::to_underlying(InvokeFlag::TryInvoke), std::span<uint32_t>{},
              output_return_buffer_len, output_returned_auth_token_len);

          return {};
        });
    auto contract_caller =
        std::make_unique<NativeContract>(std::move(fns_caller));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(contract_writer));
    contract_map.emplace(1, std::move(contract_caller));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {addr_writer, addr_caller};
  for (int i = 0; i < 2; ++i) {
    auto contract_xdr = make_native_contract(i);
    write_contract(contract_xdr);
    write_deployment(addrs[i], contract_xdr);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  // Tx 1: caller try_invokes writer:0 (writes to "test", acquires lock)
  auto handler1 = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = addr_caller,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}}},
      *context);

  auto res1 = handler1.root_invoke();
  ASSERT_TRUE(res1.is_root_success());
  // Don't commit — keeps the exclusive lock held

  // Tx 2: same invocation with tracing (try_invoke fails due to lock)
  auto handler2 = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = addr_caller,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}}},
      *context);
  handler2.enable_tracing();

  auto res2 = handler2.root_invoke();
  ASSERT_TRUE(res2.is_root_success());

  auto const *trace = handler2.get_trace();
  ASSERT_NE(trace, nullptr);

  // Root frame is the caller contract
  ASSERT_EQ(trace->invocation.invoked_address, addr_caller);

  // Root frame should have terminated with Return (caller handled try_invoke
  // failure)
  ASSERT_EQ(trace->status.status_code(), InvocationFrameStatusCode::Return);

  // Find the Invoke syscall in the root frame
  const SyscallTrace *invoke_syscall = nullptr;
  for (auto const &sc : trace->system_calls) {
    if (sc.body.syscall_number() == SyscallNo::kInvoke) {
      invoke_syscall = &sc;
      break;
    }
  }
  ASSERT_NE(invoke_syscall, nullptr);

  // Invoke should report kSyscallErrorCodeDeterministicWasm (lock failure
  // converted the child result to Deterministic)
  ASSERT_EQ(invoke_syscall->status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeDeterministicWasm);

  // Should have a child trace
  auto const &invoke_trace = invoke_syscall->body.invoke();
  ASSERT_NE(invoke_trace.outputs, nullptr);
  ASSERT_NE(invoke_trace.outputs->child_trace, nullptr);

  auto const &child_trace = *invoke_trace.outputs->child_trace;

  // Child frame is the writer contract
  ASSERT_EQ(child_trace.invocation.invoked_address, addr_writer);

  // Child trace shows Return — the child's deterministic execution succeeded,
  // but the trace status is reported as the child's execution result, before
  // the lock acquisition check fails and converts the frame result to
  // Deterministic
  ASSERT_EQ(child_trace.status.status_code(),
            InvocationFrameStatusCode::Return);
}

TEST_F(TracingTest, RcqInsertOversizedEntryWithTracing)
{
  auto contract_addr = make_contract_address(0);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    StorageKeyContractData key{};
    std::array<std::byte, kQueueEntryMaxLen + 1> oversized{};
    auto r = ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
        key, std::span<const std::byte>(oversized));
    if (!r.has_value() ||
        r.value() != SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
      ADD_FAILURE() << "Expected kSyscallErrorCodeInvalidSyscallInput for "
                       "oversized RCQ entry";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr = make_native_contract(0);
  write_contract(contract_xdr);
  write_deployment(contract_addr, contract_xdr);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}}},
      *runtime_context);
  handler.enable_tracing();
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
