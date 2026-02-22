#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "rpc_types.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// End-to-end test: run a transaction in proposal mode, capture the
// NondeterministicProposalData, then replay in validation mode and verify
// the results match.  Also test that tampered proposal data causes
// validation failure.
class ProposeValidateRoundtripTest : public ::testing::TestWithParam<TestDB>,
                                     public NativeContractTestFramework {
public:
  ProposeValidateRoundtripTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override
  {
    // Deploy two native contracts
    write_contract(native_contract_a_xdr_);
    write_deployment(kAddrA, native_contract_a_xdr_);
    setup_grant_storage_token(kAddrA, 1'000'000'000);

    write_contract(native_contract_b_xdr_);
    write_deployment(kAddrB, native_contract_b_xdr_);
    setup_grant_storage_token(kAddrB, 1'000'000'000);
  }

  static constexpr ContractAddress kAddrA = make_contract_address(0);
  static constexpr ContractAddress kAddrB = make_contract_address(1);

  std::shared_ptr<Contract> native_contract_a_xdr_ = make_native_contract(0);
  std::shared_ptr<Contract> native_contract_b_xdr_ = make_native_contract(1);
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// Contract A method 0: try_invoke Contract B method 0, then try_invoke
// Contract B method 1.  Both succeed, producing 2 nondeterministic bits
// (both true).  Nondeterministic bits are only logged when the inner frame
// succeeds (deterministic errors skip lock acquisition entirely).
//
// Contract B method 0: writes bytestring "hello" to storage.
// Contract B method 1: writes bytestring "world" to storage.
TEST_P(ProposeValidateRoundtripTest, SuccessfulRoundtrip)
{
  auto native_contract_a = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // try_invoke B method 0 — should succeed
             auto [r0, _g0] = handler.invoke(
                 kAddrB, 0, runtime.get_available_gas(), Calldata{},
                 std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // try_invoke B method 1 — should also succeed
             auto [r1, _g1] = handler.invoke(
                 kAddrB, 1, runtime.get_available_gas(), Calldata{},
                 std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r1.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Write something to prove we survived both try_invokes
             auto key = make_storage_key("result");
             auto [ws, _wg] = syscall_BytestringSet_inner(
                 handler, key, std::as_bytes(std::span(std::string_view("ok"))),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
             if (!ws.has_value() ||
                 ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
      });

  auto native_contract_b = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("b_data0");
             auto [ws, _wg] = syscall_BytestringSet_inner(
                 handler, key,
                 std::as_bytes(std::span(std::string_view("hello"))),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
             if (!ws.has_value() ||
                 ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("b_data1");
             auto [ws, _wg] = syscall_BytestringSet_inner(
                 handler, key,
                 std::as_bytes(std::span(std::string_view("world"))),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
             if (!ws.has_value() ||
                 ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_a));
  contract_map.emplace(1, std::move(native_contract_b));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // === Phase 1: Proposal mode ===
  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx, *native_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success())
        << "Proposal mode root_invoke should succeed";

    proposal_data = handler.get_proposal_data();

    // The transaction had 2 try_invokes, so there should be 2 bits
    EXPECT_EQ(proposal_data.nondeterministic_bit_count, 2U);

    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }

  // Discard the block and re-open at the same dbtip so validation sees
  // the same snapshot as the proposal.
  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // === Phase 2: Validation mode with correct proposal data ===
  {
    auto handler = make_new_system_call_handler_validation(tx, *native_context,
                                                           proposal_data);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success())
        << "Validation with correct proposal data should succeed";

    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
}

// Test that validation fails when the proposal data has extra unconsumed
// nondeterministic bits (entropy grinding attack vector).
TEST_P(ProposeValidateRoundtripTest, ExtraBitsCauseValidationFailure)
{
  auto native_contract_a = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // Single try_invoke that succeeds: produces 1 bit
             auto [r0, _g0] = handler.invoke(
                 kAddrB, 0, runtime.get_available_gas(), Calldata{},
                 std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
      });

  auto native_contract_b = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("b_data");
             auto [ws, _wg] = syscall_BytestringSet_inner(
                 handler, key,
                 std::as_bytes(std::span(std::string_view("hello"))),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
             if (!ws.has_value() ||
                 ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_a));
  contract_map.emplace(1, std::move(native_contract_b));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // === Phase 1: Proposal ===
  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx, *native_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    proposal_data = handler.get_proposal_data();
    EXPECT_EQ(proposal_data.nondeterministic_bit_count, 1U);
    ASSERT_TRUE(handler.abort_transaction().has_value());
  }

  // === Phase 2: Tamper — add extra nondeterministic bits ===
  {
    auto tampered = proposal_data;
    // Add an extra bit: now bit_count=2 but execution only consumes 1
    tampered.nondeterministic_bit_count = 2;

    auto handler = make_new_system_call_handler_validation(tx, *native_context,
                                                           std::move(tampered));
    auto res = handler.root_invoke();
    // check_all_consumed() should fail because 2 bits were supplied but
    // only 1 was consumed.
    EXPECT_FALSE(res.is_root_success())
        << "Validation should fail with extra unconsumed bits";
    ASSERT_TRUE(handler.abort_transaction().has_value());
  }
}

// Test that validation fails when proposal data has a flipped bit.
// Contract A does two try_invokes, but only does the second if the first
// succeeds.  In proposal mode both succeed, producing 2 bits (both true).
// Flipping bit 0 to false causes the validator to skip the second
// try_invoke, consuming only 1 of 2 bits → check_all_consumed() fails.
TEST_P(ProposeValidateRoundtripTest, FlippedBitsCauseValidationFailure)
{
  auto native_contract_a = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             // try_invoke B method 0
             auto [r0, _g0] = handler.invoke(
                 kAddrB, 0, runtime.get_available_gas(), Calldata{},
                 std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
             if (!r0.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // Only do the second try_invoke if the first succeeded
             if (r0.value() == SyscallErrorCode::kSyscallErrorCodeNone) {
               auto [r1, _g1] = handler.invoke(
                   kAddrB, 1, runtime.get_available_gas(), Calldata{},
                   std::to_underlying(InvokeFlag::kInvokeFlagTryInvoke));
               if (!r1.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
             }

             return {};
           }},
      });

  auto native_contract_b = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("b_data0");
             auto [ws, _wg] = syscall_BytestringSet_inner(
                 handler, key,
                 std::as_bytes(std::span(std::string_view("hello"))),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
             if (!ws.has_value() ||
                 ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto key = make_storage_key("b_data1");
             auto [ws, _wg] = syscall_BytestringSet_inner(
                 handler, key,
                 std::as_bytes(std::span(std::string_view("world"))),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
             if (!ws.has_value() ||
                 ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_a));
  contract_map.emplace(1, std::move(native_contract_b));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // === Phase 1: Proposal ===
  NondeterministicProposalData proposal_data;
  {
    auto handler = make_new_system_call_handler(tx, *native_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    proposal_data = handler.get_proposal_data();
    ASSERT_EQ(proposal_data.nondeterministic_bit_count, 2U);
    ASSERT_TRUE(handler.abort_transaction().has_value());
  }

  // === Phase 2: Tamper — flip bit 0 from true to false ===
  // With bit 0 = false, the validator treats the first try_invoke as
  // failed, so contract A skips the second try_invoke. Only 1 of the
  // 2 nondeterministic bits is consumed → check_all_consumed() fails.
  {
    auto tampered = proposal_data;
    tampered.nondeterministic_bits[0] ^= 1;

    discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

    auto handler = make_new_system_call_handler_validation(tx, *native_context,
                                                           std::move(tampered));
    auto res = handler.root_invoke();
    // The root invoke succeeds (contract A returns Ok), but
    // check_final_validation_data detects the unconsumed bit.
    if (res.is_root_success()) {
      auto commit_res = handler.try_commit_transaction();
      EXPECT_NE(commit_res, ExecResult::ExecResultOK)
          << "Validation with flipped bits should not commit successfully";
    }
    else {
      // Root invoke failed — also acceptable
      ASSERT_TRUE(handler.abort_transaction().has_value());
    }
  }
}

// Test that validation fails when the proposal is structurally malformed
// (e.g. bit count doesn't match byte count).
TEST_P(ProposeValidateRoundtripTest, MalformedProposalRejectedEarly)
{
  auto native_contract_a = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler & /*handler*/,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             // Simple no-op — just return success
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_a));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // Create malformed proposal: claim 10 bits but provide 0 bytes
  NondeterministicProposalData malformed;
  malformed.nondeterministic_bit_count = 10;
  malformed.nondeterministic_bits.clear(); // 0 bytes, but claims 10 bits

  auto handler = make_new_system_call_handler_validation(tx, *native_context,
                                                         std::move(malformed));
  auto res = handler.root_invoke();
  // validate() should reject this in perform_initial_validation
  EXPECT_FALSE(res.is_root_success())
      << "Malformed proposal (bit count/byte count mismatch) should be "
         "rejected";
  ASSERT_TRUE(handler.abort_transaction().has_value());
}

// Test that validation fails when there are padding bits set in the last
// byte (entropy grinding vector).
TEST_P(ProposeValidateRoundtripTest, PaddingBitEntropyGrinding)
{
  auto native_contract_a = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler & /*handler*/,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> { return {}; }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_a));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  Transaction tx{
      .invocation = Invocation{.invoked_address = kAddrA,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
  };

  // Create proposal with 1 bit but set high bits in the byte (padding
  // should be zero to prevent entropy grinding).
  NondeterministicProposalData grinding;
  grinding.nondeterministic_bit_count = 1;
  grinding.nondeterministic_bits = {
      0xFF}; // high 7 bits are padding, should be 0

  auto handler = make_new_system_call_handler_validation(tx, *native_context,
                                                         std::move(grinding));
  auto res = handler.root_invoke();
  EXPECT_FALSE(res.is_root_success())
      << "Padding bits set in last byte should be rejected (entropy "
         "grinding)";
  ASSERT_TRUE(handler.abort_transaction().has_value());
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, ProposeValidateRoundtripTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
