#include <gtest/gtest.h>

// End-to-end rv32 coverage for #2327, driven through the
// invoke_malformed_tokens_test contract (local_test_contracts).
//
// An Invoke whose owned-auth-token byte span is malformed (not a whole number
// of 4-byte ids) is rejected by the span-length check, which runs ahead of
// admission; admission runs ahead of the host's copy of the caller's calldata
// (#2973, pinned natively in invoke_error_handling_test.cc). A rejected Invoke
// bills nothing beyond the flat syscall crossing, so that order is what keeps
// a contract from buying host copy work for free with a one-byte token span
// and as much calldata as it likes.
//
// The guest asserts the error precedence (kInvalidSyscallInput beats
// kReentrance) and that the rejection left its outputs untouched. The trace
// assertions here add what the guest cannot see: no child frame was attached
// to the rejected record, the rejection cost exactly kSyscallBaseCost -- the
// crossing every syscall pays, with nothing that grows with the 16 KiB of
// calldata on top -- and the losing input, the full calldata and the one-byte
// span, was what the host saw. The control shows the same calldata does reach
// a child frame once the span is well-formed.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "built_contracts/invoke_malformed_tokens_test.h"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"

namespace hogd {

namespace {

// Mirrors LARGE_CALLDATA_LEN / LARGE_CALLDATA in the contract.
constexpr size_t kLargeCalldataLen = 16 * 1024;
constexpr std::uint8_t kLargeCalldataByte = 0x5a;

// The root frame's one kInvoke record. Every entrypoint here issues exactly
// one Invoke, so a second record (or none) is a failure, not a search miss.
SyscallTrace const *
single_invoke_record(InvocationFrameTrace const &trace)
{
  SyscallTrace const *found = nullptr;
  for (auto const &call : trace.system_calls) {
    if (call.body.syscall_number() != SyscallNo::kInvoke) {
      continue;
    }
    if (found != nullptr) {
      ADD_FAILURE() << "more than one kInvoke record in the root frame";
      return nullptr;
    }
    found = &call;
  }
  if (found == nullptr) {
    ADD_FAILURE() << "no kInvoke record in the root frame";
  }
  return found;
}

bool
is_large_calldata(auto const &bytes)
{
  return bytes.size() == kLargeCalldataLen &&
         std::ranges::all_of(bytes, [](std::uint8_t b) {
           return b == kLargeCalldataByte;
         });
}

// A malformed-span rejection at `address`: the documented no-op. No child
// frame, only the crossing billed, and the losing input traced in full.
void
expect_rejected_before_child_frame(SyscallTrace const &record,
                                   ContractAddress const &address)
{
  EXPECT_EQ(record.status.error(), HostFnErrorCode::NoneOrRecoverable);
  EXPECT_EQ(record.status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
  EXPECT_EQ(record.body.invoke().outputs, nullptr);
  // The body reports gas 0; wrap_after_parse() adds the crossing. Anything
  // more would be the calldata being priced, or copied.
  EXPECT_EQ(record.gas_remaining_at_end,
            record.gas_remaining_at_start - kSyscallBaseCost);

  auto const &inputs = record.body.invoke().inputs;
  EXPECT_EQ(inputs.address, address);
  EXPECT_TRUE(is_large_calldata(inputs.calldata));
  EXPECT_EQ(inputs.owned_auth_token_ids_list.size(), 1U);
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
class InvokeMalformedTokensRv32Test : public ::testing::Test,
                                      public NativeContractTestFramework {
public:
  InvokeMalformedTokensRv32Test()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

protected:
  using contract_meta = ContractTraitMeta<
      invoke_malformed_tokens_test::InvokeMalformedTokensTest>;

  void SetUp() override
  {
    auto contract = contract_meta::load_rv32();
    write_contract(contract);
    write_deployment(kContractAddress, contract);
    setup_grant_storage_token(kContractAddress, 1'000'000'000);

    runtime_context_ = make_hog_exec_context();
  }

  // Deploys a second copy of the contract, for tests whose entrypoint invokes
  // an address other than its own: the reentrance guard rejects an invoke of
  // a contract already on the invocation stack.
  void deploy_target()
  {
    auto contract = contract_meta::load_rv32();
    write_contract(contract);
    write_deployment(kTargetAddress, contract);
    setup_grant_storage_token(kTargetAddress, 1'000'000'000);
  }

  constexpr static ContractAddress kContractAddress = make_contract_address(0);
  constexpr static ContractAddress kTargetAddress = make_contract_address(1);

  // Finite, so the trace's gas figures are meaningful; far more than the
  // frames here use.
  constexpr static GasAmount kGasLimit = 1'000'000;

  ContractTraitClient<invoke_malformed_tokens_test::InvokeMalformedTokensTest>
      client_ = ContractTraitClient<
          invoke_malformed_tokens_test::InvokeMalformedTokensTest>(
          kContractAddress);

  std::unique_ptr<RuntimeContext> runtime_context_;
};

// The span check beats admission: a self-invoke that admission would refuse
// as kReentrance reports kInvalidSyscallInput when its token span is
// malformed. Admission sits ahead of the calldata copy, so this is the
// ordering #2327 asked for, seen through the one thing a guest can observe.
TEST_F(InvokeMalformedTokensRv32Test, MalformedTokensBeatReentrance)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation = client_.make_invocation<
                      u8"test_malformed_tokens_beat_reentrance">(kGasLimit),
                  .uniquifier = make_new_uniquifier()},
      *runtime_context_);
  handler.enable_tracing();
  ASSERT_TRUE(handler.root_invoke()().is_root_success());

  ASSERT_NE(handler.get_trace(), nullptr);
  auto const *record = single_invoke_record(*handler.get_trace());
  ASSERT_NE(record, nullptr);
  expect_rejected_before_child_frame(*record, kContractAddress);
}

// Against a deployable target, the same input is still refused before any
// child frame starts: nothing about the target rescues a malformed span.
TEST_F(InvokeMalformedTokensRv32Test, MalformedTokensRejectBeforeChildFrame)
{
  deploy_target();
  auto handler = make_new_system_call_handler(
      Transaction{.invocation = client_.make_invocation<
                      u8"test_malformed_tokens_reject_before_child_frame">(
                      kGasLimit, kTargetAddress),
                  .uniquifier = make_new_uniquifier()},
      *runtime_context_);
  handler.enable_tracing();
  ASSERT_TRUE(handler.root_invoke()().is_root_success());

  ASSERT_NE(handler.get_trace(), nullptr);
  auto const *record = single_invoke_record(*handler.get_trace());
  ASSERT_NE(record, nullptr);
  expect_rejected_before_child_frame(*record, kTargetAddress);
}

// Control: with a well-formed (empty) token span the same calldata reaches a
// child frame at the target, which aborts on the unregistered entrypoint id.
// Without this, the rejections above would read the same if the span check
// swallowed every Invoke that carried this calldata.
TEST_F(InvokeMalformedTokensRv32Test, WellFormedTokensRunChild)
{
  deploy_target();
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_well_formed_tokens_run_child">(
                  kGasLimit, kTargetAddress),
          .uniquifier = make_new_uniquifier()},
      *runtime_context_);
  handler.enable_tracing();
  ASSERT_TRUE(handler.root_invoke()().is_root_success());

  ASSERT_NE(handler.get_trace(), nullptr);
  auto const *record = single_invoke_record(*handler.get_trace());
  ASSERT_NE(record, nullptr);

  EXPECT_EQ(record->status.error(), HostFnErrorCode::NoneOrRecoverable);
  EXPECT_EQ(record->status.syscall_return_code(),
            SyscallErrorCode::kSyscallErrorCodeNone);
  // The kInvoke record's gas delta is the crossing plus the child frame's
  // total consumption.
  EXPECT_LT(record->gas_remaining_at_end,
            record->gas_remaining_at_start - kSyscallBaseCost);

  auto const &inputs = record->body.invoke().inputs;
  EXPECT_EQ(inputs.address, kTargetAddress);
  EXPECT_TRUE(is_large_calldata(inputs.calldata));
  EXPECT_TRUE(inputs.owned_auth_token_ids_list.empty());

  ASSERT_NE(record->body.invoke().outputs, nullptr);
  auto const &child = record->body.invoke().outputs->child_trace;
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(child->invocation.invoked_address, kTargetAddress);
  EXPECT_TRUE(is_large_calldata(child->invocation.calldata));
  EXPECT_EQ(child->status.status_code(), InvocationFrameStatusCode::Abort);
}

// #4174: a rejected Invoke that forwards a well-formed but unowned token span is
// billed for parsing it. The host copies the span into a vector and SipHashes
// every id before admission can see the ids are not owned, so that work grows
// with the span length; pricing it keeps a contract from forcing an arbitrarily
// long parse for the flat crossing charge. Two unowned spans of different
// lengths, both refused at admission, therefore differ in gas by exactly the
// parse charge for the extra bytes.
TEST_F(InvokeMalformedTokensRv32Test, UnownedTokensAreBilledByLength)
{
  deploy_target();

  struct Rejection {
    GasAmount charge;
    size_t token_bytes;
  };

  auto run = [this](auto invocation) -> Rejection {
    auto handler = make_new_system_call_handler(
        Transaction{.invocation = std::move(invocation),
                    .uniquifier = make_new_uniquifier()},
        *runtime_context_);
    handler.enable_tracing();
    EXPECT_TRUE(handler.root_invoke()().is_root_success());
    auto const *trace = handler.get_trace();
    EXPECT_NE(trace, nullptr);
    if (trace == nullptr) {
      return Rejection{0, 0};
    }
    auto const *record = single_invoke_record(*trace);
    EXPECT_NE(record, nullptr);
    if (record == nullptr) {
      return Rejection{0, 0};
    }
    // Rejected at admission (ids the frame does not own): no child frame, and
    // the caller's outputs are left untouched.
    EXPECT_EQ(record->status.error(), HostFnErrorCode::NoneOrRecoverable);
    EXPECT_EQ(record->status.syscall_return_code(),
              SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
    EXPECT_EQ(record->body.invoke().outputs, nullptr);
    return Rejection{
        record->gas_remaining_at_start - record->gas_remaining_at_end,
        record->body.invoke().inputs.owned_auth_token_ids_list.size()};
  };

  auto const one = run(
      client_.make_invocation<u8"test_one_unowned_token">(kGasLimit,
                                                          kTargetAddress));
  auto const many = run(
      client_.make_invocation<u8"test_many_unowned_tokens">(kGasLimit,
                                                            kTargetAddress));

  // Mirrors ONE_TOKEN_BYTES / MANY_TOKEN_COUNT in the contract.
  constexpr size_t kOneTokenBytes = sizeof(uint32_t);
  constexpr size_t kManyTokenBytes = 512 * sizeof(uint32_t);
  EXPECT_EQ(one.token_bytes, kOneTokenBytes);
  EXPECT_EQ(many.token_bytes, kManyTokenBytes);

  // Each charge is the flat crossing plus the parse, priced like a bytestring
  // copy of the forwarded bytes.
  auto const parse_charge = [](size_t bytes) {
    return kBytestringReadBlockCost * GasCost::bytestring_blocks(bytes);
  };
  EXPECT_EQ(one.charge, kSyscallBaseCost + parse_charge(kOneTokenBytes));
  EXPECT_EQ(many.charge, kSyscallBaseCost + parse_charge(kManyTokenBytes));
  // The point of the fix: the charge scales with the span, so the many-id span
  // costs strictly more than the one-id span rather than the same flat amount.
  EXPECT_GT(many.charge, one.charge);
}

} // namespace hogd
