#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>

#include "built_contracts/syscall_misc_tests.h"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "invoke_args.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/format.h"
#include "x23/vec.h"

namespace hogd {

namespace {

using SyscallTraceVec = x23::Vec<SyscallTrace, 0xffffffff>;

HostFunctionStatusXdr
status_with(SyscallErrorCode code)
{
  HostFunctionStatusXdr status(HostFnErrorCode::NoneOrRecoverable);
  status.syscall_return_code() = code;
  return status;
}

SyscallTrace
make_entry(GasAmount gas_at_start, GasAmount gas_at_end, SyscallTraceBody body,
           HostFunctionStatusXdr status)
{
  return SyscallTrace{
      .gas_remaining_at_start = gas_at_start,
      .gas_remaining_at_end = gas_at_end,
      .body = std::move(body),
      .status = std::move(status),
  };
}

SyscallTrace
get_calldata_entry(GasAmount gas, Calldata const &calldata)
{
  SyscallTraceBody body(SyscallNo::kGetCalldata);
  SyscallGetCalldataOutputs outputs;
  outputs.calldata.append_range(calldata);
  body.get_calldata().outputs = std::move(outputs);
  return make_entry(gas, gas, std::move(body),
                    status_with(SyscallErrorCode::kSyscallErrorCodeNone));
}

SyscallTrace
empty_return_entry(GasAmount gas)
{
  SyscallTraceBody body(SyscallNo::kReturn);
  body.return_trace().inputs.revert_frame = false;
  return make_entry(gas, gas, std::move(body),
                    HostFunctionStatusXdr(HostFnErrorCode::Return));
}

} // namespace

// Trace pins for syscalls that FAIL: the entry records the losing input, the
// winning error code from the precedence order, and (on error) no outputs.
// Companions to the guest-level precedence pins in syscall_misc_tests.
// NOLINTNEXTLINE(misc-use-internal-linkage)
class ErrorTraceTest : public ::testing::Test,
                       public NativeContractTestFramework {
public:
  ErrorTraceTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

protected:
  Rv32Context runtime_context_;

  constexpr static GasAmount kGasLimit = 100'000;

  ContractAddress deploy_misc_contract()
  {
    auto contract =
        ContractTraitMeta<syscall_misc_tests::SyscallMiscTests>::load_rv32();
    auto addr = make_contract_address(0);
    write_contract(contract);
    write_deployment(addr, contract);
    setup_grant_storage_token(addr, 1'000'000'000);
    return addr;
  }
};

TEST_F(ErrorTraceTest, GetReturnedBufferPrecedenceTraceIsExact)
{
  auto addr = deploy_misc_contract();
  ContractTraitClient<syscall_misc_tests::SyscallMiscTests> client(addr);
  const SignedTransaction tx{
      .tx = Transaction{
          .invocation = client.make_invocation<
              u8"test_get_returned_buffer_no_buffer_beats_misaligned">(
              kGasLimit),
          .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(tx, runtime_context_);
  handler.enable_tracing();
  handler.enable_transaction_stats();
  ASSERT_TRUE(handler.root_invoke().is_root_success());

  SyscallTraceVec expected;
  expected.push_back(get_calldata_entry(99'350, tx.tx.invocation.calldata));
  {
    // No child frame ran, and the restored-token span is misaligned (3
    // bytes); the buffer-availability check wins and no outputs are written.
    SyscallTraceBody body(SyscallNo::kGetReturnedBuffer);
    expected.push_back(make_entry(
        99'227, 99'227, std::move(body),
        status_with(
            SyscallErrorCode::kSyscallErrorCodeNoReturnBufferAvailable)));
  }
  expected.push_back(empty_return_entry(99'195));

  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);
}

TEST_F(ErrorTraceTest, UnownedTokenBeatsReentranceTraceIsExact)
{
  auto addr = deploy_misc_contract();
  ContractTraitClient<syscall_misc_tests::SyscallMiscTests> client(addr);
  const SignedTransaction tx{
      .tx = Transaction{.invocation = client.make_invocation<
                            u8"test_invoke_unowned_token_beats_reentrance">(
                            kGasLimit),
                        .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(tx, runtime_context_);
  handler.enable_tracing();
  handler.enable_transaction_stats();
  ASSERT_TRUE(handler.root_invoke().is_root_success());

  SyscallTraceVec expected;
  expected.push_back(get_calldata_entry(99'370, tx.tx.invocation.calldata));
  {
    SyscallTraceBody body(SyscallNo::kGetCurrentAddress);
    body.get_current_address().outputs =
        SyscallGetCurrentAddressOutputs{.address = addr};
    expected.push_back(
        make_entry(99'336, 99'336, std::move(body),
                   status_with(SyscallErrorCode::kSyscallErrorCodeNone)));
  }
  {
    // Self-invoke forwarding a fabricated token id 0xDEAD: the ownership
    // check wins over the reentrance check, and no child frame runs.
    SyscallTraceBody body(SyscallNo::kInvoke);
    body.invoke().inputs.address = addr;
    body.invoke().inputs.owned_auth_token_ids_list.append_range(
        std::initializer_list<uint8_t>{0xad, 0xde, 0x00, 0x00});
    body.invoke().inputs.inputs = CopyableInvokeInputs{
        .entrypoint_id = 0,
        .gas_limit = UINT64_MAX,
        .flags = 0,
    };
    expected.push_back(make_entry(
        99'302, 99'302, std::move(body),
        status_with(SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput)));
  }
  expected.push_back(empty_return_entry(99'264));

  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);
}

} // namespace hogd
