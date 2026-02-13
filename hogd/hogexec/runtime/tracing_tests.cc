#include <cstdint>
#include <gtest/gtest.h>

#include <memory>

#include "built_contracts/contract_loader.h"
#include "built_contracts/fibonacci_call.h"
#include "built_contracts/no_syscall_test.h"
#include "built_contracts/sha256_wrapper.h"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "x23/vec.h"

namespace hogd {

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
              HostFnErrorCode::kHostNoneOrRecoverable);
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
  ASSERT_EQ(trace->status.status_code(),
            InvocationFrameStatusCode::kInvocationFrameStatusCodeReturn);

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
            InvocationFrameStatusCode::kInvocationFrameStatusCodeReturn);

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

} // namespace hogd
