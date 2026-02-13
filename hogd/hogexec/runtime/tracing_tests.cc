#include <gtest/gtest.h>

#include <memory>

#include "built_contracts/contract_loader.h"
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
    ASSERT_EQ(trace_call.output.error(), SyscallErrorCode::kSyscallErrorCodeNone);
    ASSERT_EQ(trace_call.input, data);
    ASSERT_EQ(trace_call.output.success().hash, sha256(data));
    ASSERT_EQ(trace->system_calls[i].status.error(),
              HostFnErrorCode::kHostNoneOrRecoverable);
    ASSERT_EQ(trace->system_calls[i].status.syscall_return_code(),
              SyscallErrorCode::kSyscallErrorCodeNone);
  }

  ASSERT_EQ(trace->system_calls[n + 1].body.syscall_number(),
            SyscallNo::kReturn);
}

} // namespace hogd
