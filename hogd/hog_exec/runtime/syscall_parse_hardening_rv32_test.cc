#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

#include "built_contracts/syscall_parse_hardening_test.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

// Tests that invoke the syscall parse handlers with various edge cases
// and assert that those edge cases return kFault properly.

namespace hogd {

class SyscallParseHardeningRv32Test : public ::testing::Test,
                                      public NativeContractTestFramework {
public:
  SyscallParseHardeningRv32Test()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

protected:
  void SetUp() override
  {
    using syscall_parse_hardening_test_meta = ContractTraitMeta<
        syscall_parse_hardening_test::SyscallParseHardeningTest>;

    auto contract = syscall_parse_hardening_test_meta::load_rv32();
    write_contract(contract);
    write_deployment(kContractAddress, contract);
    setup_grant_storage_token(kContractAddress, 1'000'000'000);

    runtime_context_ = make_hog_exec_context();
  }

  constexpr static ContractAddress kContractAddress = make_contract_address(0);

  ContractTraitClient<syscall_parse_hardening_test::SyscallParseHardeningTest>
      client_ = ContractTraitClient<
          syscall_parse_hardening_test::SyscallParseHardeningTest>(
          kContractAddress);

  std::unique_ptr<RuntimeContext> runtime_context_;

  uint64_t nonce_ = 0;
};

// Can't turn value that doesn't fit into an int32_t into a CryptographicScheme
// enum (i32)
TEST_F(SyscallParseHardeningRv32Test, VerifySignatureRejectsNoncanonicalScheme)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation = client_.make_invocation<
              u8"test_verify_signature_noncanonical_scheme_fault">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// OOB optional pointer that's not u64 max must fault in syscall parsing.
TEST_F(SyscallParseHardeningRv32Test, ChangePubkeyOptionalPointerFaults)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation = client_.make_invocation<
              u8"test_change_pubkey_optional_pointer_fault">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// only uint64_max is a valid sentinel for optional pointer
TEST_F(SyscallParseHardeningRv32Test,
       GetDeployAddressOptionalPointerSentinelBehavior)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation = client_.make_invocation<
                      u8"test_get_deploy_address_optional_pointer_sentinel">(
                      UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// The signature half of VerifySignature's size check: 63-byte signature with
// a valid key is the inner kInvalidSyscallInput, not a parse fault.
TEST_F(SyscallParseHardeningRv32Test, VerifySignatureRejectsWrongSizeSignature)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation = client_.make_invocation<
              u8"test_verify_signature_wrong_size_signature">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// A zero-length span with an out-of-range pointer faults; the valid-pointer
// control succeeds.
TEST_F(SyscallParseHardeningRv32Test, ZeroLenSpanOobPointerFaults)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_zero_len_span_oob_pointer_fault">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// The span end (ptr + len) must be computed wider than 32 bits; a u32-wrapping
// sum would land back in bounds and accept the span.
TEST_F(SyscallParseHardeningRv32Test, SpanEndU32WrapFaults)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_span_end_u32_wrap_fault">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

} // namespace hogd
