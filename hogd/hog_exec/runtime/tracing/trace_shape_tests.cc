#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "built_contracts/ed25519_verify_wrapper.h"
#include "built_contracts/fibonacci.h"
#include "built_contracts/fibonacci_call.h"
#include "built_contracts/manual_revert_test.h"
#include "built_contracts/no_syscall_test.h"
#include "built_contracts/read_only_test.h"
#include "built_contracts/sha256_wrapper.h"
#include "event.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
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
return_entry(GasAmount gas, std::span<const uint8_t> buffer,
             bool revert_frame = false)
{
  SyscallTraceBody body(SyscallNo::kReturn);
  body.return_trace().inputs.buffer.append_range(buffer);
  body.return_trace().inputs.revert_frame = revert_frame;
  return make_entry(gas, gas, std::move(body),
                    HostFunctionStatusXdr(HostFnErrorCode::Return));
}

SyscallTrace
bytestring_get_len_entry(GasAmount gas, StorageKeyContractData const &key,
                         uint32_t length)
{
  SyscallTraceBody body(SyscallNo::kBytestringGetLen);
  body.bytestring_get_len().inputs.key = key;
  body.bytestring_get_len().outputs =
      SyscallBytestringGetLenOutputs{.length = length};
  return make_entry(gas, gas, std::move(body), ok_status());
}

SyscallTrace
bytestring_get_entry(GasAmount gas, StorageKeyContractData const &key,
                     std::span<const uint8_t> value)
{
  SyscallTraceBody body(SyscallNo::kBytestringGet);
  body.bytestring_get().inputs.key = key;
  SyscallBytestringGetOutputs outputs;
  outputs.bytestring.append_range(value);
  body.bytestring_get().outputs = std::move(outputs);
  return make_entry(gas, gas, std::move(body), ok_status());
}

SyscallTrace
get_returned_buffer_entry(GasAmount gas, std::span<const uint8_t> buffer)
{
  SyscallTraceBody body(SyscallNo::kGetReturnedBuffer);
  SyscallGetReturnedBufferOutputs outputs;
  outputs.buffer.append_range(buffer);
  body.get_returned_buffer().outputs = std::move(outputs);
  return make_entry(gas, gas, std::move(body), ok_status());
}

std::vector<uint8_t>
from_hex(std::string_view hex)
{
  std::vector<uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    auto nibble = [](char c) -> uint8_t {
      return c >= 'a' ? static_cast<uint8_t>(c - 'a' + 10)
                      : static_cast<uint8_t>(c - '0');
    };
    bytes.push_back(
        static_cast<uint8_t>((nibble(hex[i]) << 4U) | nibble(hex[i + 1])));
  }
  return bytes;
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

  template<typename Trait>
  ContractAddress deploy_sdk_contract(uint64_t addr_seed = 0)
  {
    auto contract = ContractTraitMeta<Trait>::load_rv32();
    auto addr = make_contract_address(addr_seed);
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
      .tx =
          Transaction{.invocation =
                          client.make_invocation<u8"sha256">(kGasLimit, input),
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
      99'919, static_cast<uint32_t>(tx.tx.invocation.calldata.size())));
  expected.push_back(get_calldata_entry(99'895, tx.tx.invocation.calldata));
  {
    SyscallTraceBody body(SyscallNo::kSHA256);
    body.sha256().inputs.input.append_range(input);
    body.sha256().outputs = SyscallSHA256Outputs{.hash = expected_hash};
    expected.push_back(make_entry(99'818, 99'818 - SHA256HashCost(input.size()),
                                  std::move(body), ok_status()));
  }
  expected.push_back(return_entry(98'498, expected_hash));

  ASSERT_NE(handler.get_trace(), nullptr);
  EXPECT_TRUE(handler.get_trace()->storage_writes.empty());
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);

  ASSERT_NE(handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(handler.get_transaction_stats()->invalid_syscall_count, 0);
  EXPECT_EQ(handler.get_transaction_stats()->sha256_total_bytes_processed,
            input.size());
}

TEST_F(TraceShapeTest, Sha256RepeatedMultiMethodDispatchTraceIsExact)
{
  const x23::Vec<uint8_t, 0xffffffff> input = {1, 2, 3, 4, 5, 8, 13};
  const SHA256Hash expected_hash = sha256(input);
  constexpr uint32_t kRepeat = 3;

  auto addr = deploy_sdk_contract<sha256_wrapper::Sha256Wrapper>();
  ContractTraitClient<sha256_wrapper::Sha256Wrapper> client(addr);
  const SignedTransaction tx{
      .tx =
          Transaction{.invocation = client.make_invocation<u8"sha256_repeated">(
                          kGasLimit, kRepeat, input),
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(tx, runtime_context_);
  handler.enable_tracing();
  handler.enable_transaction_stats();

  ASSERT_TRUE(handler.root_invoke().is_root_success());

  const auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(output.return_buffer().empty());

  const std::array<GasAmount, kRepeat> sha256_gas = {99'774, 98'562, 97'350};
  SyscallTraceVec expected;
  expected.push_back(get_calldata_len_entry(
      99'908, static_cast<uint32_t>(tx.tx.invocation.calldata.size())));
  expected.push_back(get_calldata_entry(99'884, tx.tx.invocation.calldata));
  for (const GasAmount gas : sha256_gas) {
    SyscallTraceBody body(SyscallNo::kSHA256);
    body.sha256().inputs.input.append_range(input);
    body.sha256().outputs = SyscallSHA256Outputs{.hash = expected_hash};
    expected.push_back(make_entry(gas, gas - SHA256HashCost(input.size()),
                                  std::move(body), ok_status()));
  }
  expected.push_back(return_entry(96'095, {}));

  ASSERT_NE(handler.get_trace(), nullptr);
  EXPECT_TRUE(handler.get_trace()->storage_writes.empty());
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);

  ASSERT_NE(handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(handler.get_transaction_stats()->invalid_syscall_count, 0);
  EXPECT_EQ(handler.get_transaction_stats()
                ->syscall_counts[std::to_underlying(SyscallNo::kSHA256)],
            kRepeat);
  EXPECT_EQ(handler.get_transaction_stats()->sha256_total_bytes_processed,
            size_t{kRepeat} * input.size());
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
          .invocation = client.make_invocation<u8"write_storage_and_log_event">(
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
      99'880, static_cast<uint32_t>(tx.tx.invocation.calldata.size())));
  expected.push_back(get_calldata_entry(99'856, tx.tx.invocation.calldata));
  {
    SyscallTraceBody body(SyscallNo::kBytestringSet);
    body.bytestring_set().inputs.key = key;
    body.bytestring_set().inputs.bytestring.append_range(value);
    body.bytestring_set().inputs.flags = 0;
    expected.push_back(
        make_entry(99'784, 99'784, std::move(body), ok_status()));
  }
  {
    SyscallTraceBody body(SyscallNo::kLogEvent);
    body.log_event().inputs.topic0 = topic0;
    body.log_event().inputs.data.append_range(data);
    expected.push_back(
        make_entry(99'774, 99'774, std::move(body), ok_status()));
  }
  expected.push_back(return_entry(99'718, {}));

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

TEST_F(TraceShapeTest, CrossCallFibonacciTraceIsExact)
{
  auto fibonacci_addr = deploy_sdk_contract<fibonacci::Fibonacci>(0);
  auto caller_addr = deploy_sdk_contract<fibonacci_call::FibonacciCaller>(1);
  ContractTraitClient<fibonacci_call::FibonacciCaller> caller(caller_addr);

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  caller.make_invocation<u8"initialize">(
                                      kGasLimit, fibonacci_addr),
                              .uniquifier = make_new_uniquifier()}},
        runtime_context_);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
    advance_block();
  }

  const SignedTransaction tx{
      .tx =
          Transaction{.invocation = caller.make_invocation<u8"call_fibonacci">(
                          kGasLimit, uint32_t{5}, uint64_t{50'000}),
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(tx, runtime_context_);
  handler.enable_tracing();
  handler.enable_transaction_stats();
  ASSERT_TRUE(handler.root_invoke().is_root_success());

  // The caller derives its storage key by hashing the SDK static_key
  // preimage (length-prefixed "FibonacciContractCaller" plus the field tag),
  // loads the stored fibonacci address, and cross-calls it.
  const std::vector<uint8_t> key_preimage =
      from_hex("18174669626f6e61636369436f6e747261637443616c6c65720100");
  const SHA256Hash storage_key = sha256(std::span<const uint8_t>(key_preimage));
  const Invocation inner =
      ContractTraitClient<fibonacci::Fibonacci>(fibonacci_addr)
          .make_invocation<u8"slow_fibonacci">(50'000, uint32_t{5});
  const auto fib_entrypoint = inner.entrypoint;
  const Calldata &inner_calldata = inner.calldata;

  SyscallTraceVec expected;
  expected.push_back(get_calldata_entry(99'918, tx.tx.invocation.calldata));
  {
    SyscallTraceBody body(SyscallNo::kSHA256);
    body.sha256().inputs.input.append_range(key_preimage);
    body.sha256().outputs = SyscallSHA256Outputs{.hash = storage_key};
    expected.push_back(make_entry(98'930,
                                  98'930 - SHA256HashCost(key_preimage.size()),
                                  std::move(body), ok_status()));
  }
  expected.push_back(bytestring_get_len_entry(95'676, storage_key, 32));
  expected.push_back(
      bytestring_get_entry(95'661, storage_key,
                           std::span<const uint8_t>(fibonacci_addr.begin(),
                                                    fibonacci_addr.end())));
  {
    SyscallTraceBody body(SyscallNo::kInvoke);
    body.invoke().inputs.address = fibonacci_addr;
    body.invoke().inputs.calldata.append_range(inner_calldata);
    body.invoke().inputs.inputs = CopyableInvokeInputs{
        .entrypoint_id = sdk_method_id(fib_entrypoint),
        .gas_limit = 50'000,
        .flags = std::to_underlying(InvokeFlag::None),
    };
    InvocationFrameTrace child;
    child.invocation = Invocation{
        .invoked_address = fibonacci_addr,
        .entrypoint = fib_entrypoint,
        .gas_limit = 50'000,
        .calldata = {},
    };
    child.invocation.calldata = inner_calldata;
    child.system_calls.push_back(get_calldata_entry(49'918, inner_calldata));
    child.system_calls.push_back(return_entry(49'658, inner_calldata));
    child.status = InvocationFrameOutput(InvocationFrameStatusCode::Return);
    child.status.return_buffer().append_range(inner_calldata);
    body.invoke().outputs =
        SyscallInvokeOutputs{.child_trace = std::move(child)};
    // The kInvoke entry's gas delta is the child frame's total consumption.
    expected.push_back(
        make_entry(95'548, 95'206, std::move(body), ok_status()));
  }
  expected.push_back(get_returned_buffer_entry(95'177, inner_calldata));
  expected.push_back(return_entry(95'067, {}));

  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);
}

TEST_F(TraceShapeTest, CrossCallOutOfGasChildTraceIsExact)
{
  auto fibonacci_addr = deploy_sdk_contract<fibonacci::Fibonacci>(0);
  auto caller_addr = deploy_sdk_contract<fibonacci_call::FibonacciCaller>(1);
  ContractTraitClient<fibonacci_call::FibonacciCaller> caller(caller_addr);

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  caller.make_invocation<u8"initialize">(
                                      kGasLimit, fibonacci_addr),
                              .uniquifier = make_new_uniquifier()}},
        runtime_context_);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
    advance_block();
  }

  const SignedTransaction tx{
      .tx = Transaction{
          .invocation =
              caller.make_invocation<u8"call_fibonacci_expect_out_of_gas">(
                  kGasLimit, uint32_t{20}, uint64_t{1000}),
          .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(tx, runtime_context_);
  handler.enable_tracing();
  handler.enable_transaction_stats();
  ASSERT_TRUE(handler.root_invoke().is_root_success());

  const std::vector<uint8_t> key_preimage =
      from_hex("18174669626f6e61636369436f6e747261637443616c6c65720100");
  const SHA256Hash storage_key = sha256(std::span<const uint8_t>(key_preimage));
  const Invocation inner =
      ContractTraitClient<fibonacci::Fibonacci>(fibonacci_addr)
          .make_invocation<u8"slow_fibonacci">(1000, uint32_t{20});
  const auto fib_entrypoint = inner.entrypoint;
  const Calldata &inner_calldata = inner.calldata;

  SyscallTraceVec expected;
  expected.push_back(get_calldata_entry(99'898, tx.tx.invocation.calldata));
  {
    SyscallTraceBody body(SyscallNo::kSHA256);
    body.sha256().inputs.input.append_range(key_preimage);
    body.sha256().outputs = SyscallSHA256Outputs{.hash = storage_key};
    expected.push_back(make_entry(98'911,
                                  98'911 - SHA256HashCost(key_preimage.size()),
                                  std::move(body), ok_status()));
  }
  expected.push_back(bytestring_get_len_entry(95'657, storage_key, 32));
  expected.push_back(
      bytestring_get_entry(95'642, storage_key,
                           std::span<const uint8_t>(fibonacci_addr.begin(),
                                                    fibonacci_addr.end())));
  {
    SyscallTraceBody body(SyscallNo::kInvoke);
    body.invoke().inputs.address = fibonacci_addr;
    body.invoke().inputs.calldata.append_range(inner_calldata);
    body.invoke().inputs.inputs = CopyableInvokeInputs{
        .entrypoint_id = sdk_method_id(fib_entrypoint),
        .gas_limit = 1000,
        .flags = std::to_underlying(InvokeFlag::None),
    };
    InvocationFrameTrace child;
    child.invocation = Invocation{
        .invoked_address = fibonacci_addr,
        .entrypoint = fib_entrypoint,
        .gas_limit = 1000,
        .calldata = {},
    };
    child.invocation.calldata = inner_calldata;
    child.system_calls.push_back(get_calldata_entry(918, inner_calldata));
    child.status = InvocationFrameOutput(InvocationFrameStatusCode::Fault);
    body.invoke().outputs =
        SyscallInvokeOutputs{.child_trace = std::move(child)};
    // The out-of-gas child consumes its entire 1000-gas limit.
    expected.push_back(
        make_entry(95'530, 94'530, std::move(body), ok_status()));
  }
  expected.push_back(return_entry(94'468, {}));

  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);
}

TEST_F(TraceShapeTest, ManualRevertAbortTraceIsExact)
{
  auto addr = deploy_sdk_contract<manual_revert_test::ManualRevertTest>();
  ContractTraitClient<manual_revert_test::ManualRevertTest> client(addr);
  const SignedTransaction tx{
      .tx =
          Transaction{.invocation =
                          client.make_invocation<u8"test_basic_manual_revert">(
                              kGasLimit, true),
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(tx, runtime_context_);
  handler.enable_tracing();
  handler.enable_transaction_stats();
  ASSERT_TRUE(handler.root_invoke().is_root_success());

  // CXS encoding of the thrown ManualRevertTestError::BasicRevert exception.
  const std::vector<uint8_t> abort_buffer = from_hex("ffb6f18780ef7b828201");
  const auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Abort);
  EXPECT_TRUE(std::ranges::equal(output.abort_buffer(), abort_buffer));

  // static_key! derivation of the contract's test storage key; the value it
  // writes before aborting is recorded in the trace even though the frame's
  // writes are discarded.
  const std::vector<uint8_t> key_bytes = from_hex(
      "c3d09081ec06ff964889b704181ca39a3398d69782d8479a434deb039b6075ec");
  StorageKeyContractData key{};
  std::ranges::copy(key_bytes, key.begin());
  const std::string_view value_str = "state_before_revert";
  const std::vector<uint8_t> value(value_str.begin(), value_str.end());

  SyscallTraceVec expected;
  expected.push_back(get_calldata_len_entry(
      99'889, static_cast<uint32_t>(tx.tx.invocation.calldata.size())));
  expected.push_back(get_calldata_entry(99'865, tx.tx.invocation.calldata));
  {
    SyscallTraceBody body(SyscallNo::kBytestringSet);
    body.bytestring_set().inputs.key = key;
    body.bytestring_set().inputs.bytestring.append_range(value);
    body.bytestring_set().inputs.flags = 0;
    expected.push_back(
        make_entry(99'761, 99'761, std::move(body), ok_status()));
  }
  expected.push_back(return_entry(99'654, abort_buffer, /*revert_frame=*/true));

  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);
  EXPECT_EQ(handler.get_trace()->status.status_code(),
            InvocationFrameStatusCode::Abort);
  EXPECT_EQ(handler.get_trace()->storage_writes.size(), 1U);
}

TEST_F(TraceShapeTest, VerifySignatureTraceIsExact)
{
  std::array<std::byte, 32> seed{};
  seed.fill(std::byte{7});
  const auto [public_key, secret_key] =
      ed25519_keygen_from_seed(std::span<const std::byte, 32>(seed));
  const x23::Vec<uint8_t, 0xffffffff> message = {0xde, 0xad, 0xbe, 0xef};
  const auto signature_opt =
      ed25519_sign(secret_key, x23::BufView(message.data(), message.size()));
  ASSERT_TRUE(signature_opt.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const Ed25519Signature signature = *signature_opt;

  auto addr =
      deploy_sdk_contract<ed25519_verify_wrapper::Ed25519VerifyWrapper>();
  ContractTraitClient<ed25519_verify_wrapper::Ed25519VerifyWrapper> client(
      addr);
  const SignedTransaction tx{
      .tx =
          Transaction{.invocation = client.make_invocation<u8"ed25519_verify">(
                          kGasLimit, message, signature, public_key),
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(tx, runtime_context_);
  handler.enable_tracing();
  handler.enable_transaction_stats();
  ASSERT_TRUE(handler.root_invoke().is_root_success());

  SyscallTraceVec expected;
  expected.push_back(get_calldata_len_entry(
      99'919, static_cast<uint32_t>(tx.tx.invocation.calldata.size())));
  expected.push_back(get_calldata_entry(99'895, tx.tx.invocation.calldata));
  {
    // Signature verification charges no direct host gas.
    SyscallTraceBody body(SyscallNo::kVerifySignature);
    body.verify_signature().inputs.scheme =
        std::to_underlying(CryptographicScheme::Ed25519);
    body.verify_signature().inputs.signature.append_range(signature);
    body.verify_signature().inputs.public_key.append_range(public_key);
    body.verify_signature().inputs.message.append_range(message);
    expected.push_back(
        make_entry(99'814, 99'814, std::move(body), ok_status()));
  }
  const std::vector<uint8_t> true_cxs = {0x01};
  expected.push_back(return_entry(99'729, true_cxs));

  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);
}

} // namespace hogd
