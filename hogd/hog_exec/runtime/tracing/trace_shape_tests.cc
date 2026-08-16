#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "built_contracts/no_syscall_test.h"
#include "built_contracts/read_only_test.h"
#include "built_contracts/sha256_wrapper.h"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/format.h"

namespace hogd {

namespace {

using SyscallTraceVec = x23::Vec<SyscallTrace, 0xffffffff>;

HostFunctionStatusXdr
ok_status()
{
  HostFunctionStatusXdr status(HostFnErrorCode::NoneOrRecoverable);
  status.syscall_return_code() = SyscallErrorCode::kSyscallErrorCodeNone;
  return status;
}

HostFunctionStatusXdr
error_status(SyscallErrorCode code)
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
get_calldata_len_entry(GasAmount gas, uint32_t len)
{
  SyscallTraceBody body(SyscallNo::kGetCalldataLen);
  body.get_calldata_len().outputs =
      SyscallGetCalldataLenOutputs{.len = len, .auth_token_count = 0};
  return make_entry(gas, gas, std::move(body), ok_status());
}

SyscallTrace
get_calldata_entry(GasAmount gas, Calldata const &calldata)
{
  SyscallTraceBody body(SyscallNo::kGetCalldata);
  SyscallGetCalldataOutputs outputs;
  outputs.calldata.append_range(calldata);
  body.get_calldata().outputs = std::move(outputs);
  return make_entry(gas, gas, std::move(body), ok_status());
}

SyscallTrace
return_entry(GasAmount gas, std::span<const uint8_t> buffer)
{
  SyscallTraceBody body(SyscallNo::kReturn);
  body.return_trace().inputs.buffer.append_range(buffer);
  body.return_trace().inputs.revert_frame = false;
  return make_entry(gas, gas, std::move(body),
                    HostFunctionStatusXdr(HostFnErrorCode::Return));
}

} // namespace

// Absolute pins of the consensus-visible syscall trace emitted by real
// SDK-compiled rv32 contracts on the production stack: the whole trace is
// compared against hardcoded expected SyscallTrace values, not against
// another runtime. The absolute gas values shift when the SDK or the gas
// schedule changes; regenerate them deliberately when that happens (the
// failure message prints the observed trace).
// NOLINTNEXTLINE(misc-use-internal-linkage)
class TraceShapeTest : public ::testing::Test,
                       public NativeContractTestFramework {
public:
  TraceShapeTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

protected:
  Rv32Context runtime_context_;

  constexpr static GasAmount kGasLimit = 100'000;

  template<typename Trait> ContractAddress deploy_sdk_contract()
  {
    auto contract = ContractTraitMeta<Trait>::load_rv32();
    auto addr = make_contract_address(0);
    write_contract(contract);
    write_deployment(addr, contract);
    setup_grant_storage_token(addr, 1'000'000'000);
    return addr;
  }
};

TEST_F(TraceShapeTest, Sha256WrapperTraceIsExact)
{
  const x23::Vec<uint8_t, 0xffffffff> input = {1, 2, 3, 4, 5, 8, 13};
  const SHA256Hash expected_hash = sha256(input);

  auto addr = deploy_sdk_contract<sha256_wrapper::Sha256Wrapper>();
  ContractTraitClient<sha256_wrapper::Sha256Wrapper> client(addr);
  const SignedTransaction tx{
      .tx = Transaction{.invocation =
                            client.make_invocation<u8"sha256">(kGasLimit,
                                                               input),
                        .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(tx, runtime_context_);
  handler.enable_tracing();
  handler.enable_transaction_stats();

  ASSERT_TRUE(handler.root_invoke().is_root_success());

  const auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(std::ranges::equal(output.return_buffer(), expected_hash));
  EXPECT_TRUE(handler.get_most_recent_frame_events().empty());

  SyscallTraceVec expected;
  expected.push_back(get_calldata_len_entry(
      99'917, static_cast<uint32_t>(tx.tx.invocation.calldata.size())));
  expected.push_back(get_calldata_entry(99'893, tx.tx.invocation.calldata));
  {
    SyscallTraceBody body(SyscallNo::kSHA256);
    body.sha256().inputs.input.append_range(input);
    body.sha256().outputs = SyscallSHA256Outputs{.hash = expected_hash};
    expected.push_back(make_entry(99'816,
                                  99'816 - SHA256HashCost(input.size()),
                                  std::move(body), ok_status()));
  }
  expected.push_back(return_entry(98'494, expected_hash));

  ASSERT_NE(handler.get_trace(), nullptr);
  EXPECT_TRUE(handler.get_trace()->storage_writes.empty());
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);

  ASSERT_NE(handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(handler.get_transaction_stats()->invalid_syscall_count, 0);
}

TEST_F(TraceShapeTest, StorageWriteAndLogEventTraceIsExact)
{
  // Constants from read_only_test's write_storage_and_log_event.
  const StorageKeyContractData key =
      make_storage_key_matching_static_key_sdk_macro("rv32_parity_key");
  const std::string_view value_str = "rv32 parity bytes";
  const std::vector<uint8_t> value(value_str.begin(), value_str.end());
  Topic topic0;
  topic0.fill(0x5a);
  const std::string_view data_str = "rv32 parity event";
  const std::vector<uint8_t> data(data_str.begin(), data_str.end());

  auto addr = deploy_sdk_contract<read_only_test::ReadOnlyTest>();
  ContractTraitClient<read_only_test::ReadOnlyTest> client(addr);
  const SignedTransaction tx{
      .tx = Transaction{
          .invocation =
              client.make_invocation<u8"write_storage_and_log_event">(
                  kGasLimit),
          .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(tx, runtime_context_);
  handler.enable_tracing();
  handler.enable_transaction_stats();

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  const auto events = handler.get_most_recent_frame_events();
  ASSERT_NE(handler.get_trace(), nullptr);
  auto const &trace = *handler.get_trace();

  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK,
                                  ProposedTransaction{.signed_tx = tx})());

  SyscallTraceVec expected;
  expected.push_back(get_calldata_len_entry(
      99'875, static_cast<uint32_t>(tx.tx.invocation.calldata.size())));
  expected.push_back(get_calldata_entry(99'851, tx.tx.invocation.calldata));
  {
    SyscallTraceBody body(SyscallNo::kBytestringSet);
    body.bytestring_set().inputs.key = key;
    body.bytestring_set().inputs.bytestring.append_range(value);
    body.bytestring_set().inputs.flags = 0;
    expected.push_back(make_entry(99'779, 99'779, std::move(body),
                                  ok_status()));
  }
  {
    SyscallTraceBody body(SyscallNo::kLogEvent);
    body.log_event().inputs.topic0 = topic0;
    body.log_event().inputs.data.append_range(data);
    expected.push_back(make_entry(99'769, 99'769, std::move(body),
                                  ok_status()));
  }
  expected.push_back(return_entry(99'706, {}));

  ASSERT_EQ(trace.system_calls, expected) << x23::show(trace.system_calls);

  // The committed storage delta and the emitted event mirror the trace.
  ASSERT_EQ(trace.storage_writes.size(), 1U);
  auto const &write = trace.storage_writes[0];
  EXPECT_EQ(write.addrkey.address, addr);
  EXPECT_EQ(write.flags, 0U);
  ASSERT_EQ(write.delta.type(), StorageDeltaType::kDeltaTypeBytestringSet);
  EXPECT_TRUE(std::ranges::equal(write.delta.bytestring(), value));

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].contract_address, addr);
  EXPECT_EQ(events[0].num_topics, 1U);
  x23::Vec<uint8_t, kMaxEventLength> expected_event;
  expected_event.append_range(topic0);
  expected_event.append_range(data);
  EXPECT_EQ(events[0].serialized_event, expected_event);
}

TEST_F(TraceShapeTest, InvalidSyscallFallbackTraceIsExact)
{
  auto addr = deploy_sdk_contract<no_syscall_test::NoSyscallTest>();
  ContractTraitClient<no_syscall_test::NoSyscallTest> client(addr);
  const SignedTransaction tx{
      .tx = Transaction{.invocation = client.make_invocation<u8"run_test">(
                            kGasLimit, static_cast<uint64_t>(kSyscallCount)),
                        .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(tx, runtime_context_);
  handler.enable_tracing();
  handler.enable_transaction_stats();

  ASSERT_TRUE(handler.root_invoke().is_root_success());

  // The SDK skips GetCalldataLen for a fixed-size (u64) calldata layout, and
  // the invalid syscall is traced under its raw out-of-range discriminant.
  SyscallTraceVec expected;
  expected.push_back(get_calldata_entry(99'938, tx.tx.invocation.calldata));
  expected.push_back(
      make_entry(99'872, 99'872 - kNoSyscallCost,
                 SyscallTraceBody(static_cast<SyscallNo>(kSyscallCount)),
                 error_status(SyscallErrorCode::kSyscallErrorCodeNoSyscall)));
  expected.push_back(return_entry(99'833, {}));

  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);

  ASSERT_NE(handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(handler.get_transaction_stats()->invalid_syscall_count, 1);
}

} // namespace hogd
