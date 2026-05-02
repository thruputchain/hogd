#include <gtest/gtest.h>

// Miscellaneous end to end tests on syscall behavior,
// implemented within a wasm contract.
// These tests are just wrappers around the tests in the syscall_misc_tests
// contract.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

#include "built_contracts/syscall_misc_tests.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class SyscallMiscTests : public ::testing::Test,
                         public NativeContractTestFramework {
public:
  SyscallMiscTests()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

protected:
  void SetUp() override
  {
    using syscall_misc_tests_meta =
        ContractTraitMeta<syscall_misc_tests::SyscallMiscTests>;

    // Load contract
    auto contract = syscall_misc_tests_meta::load_wasm();

    // Deploy contract
    write_contract(contract);
    write_deployment(kContractAddress, contract);

    // Grant storage tokens
    setup_grant_storage_token(kContractAddress, 1'000'000'000);

    runtime_context_ = make_hogexec_context();
  }

  constexpr static ContractAddress kContractAddress = make_contract_address(0);

  ContractTraitClient<syscall_misc_tests::SyscallMiscTests> client_ =
      ContractTraitClient<syscall_misc_tests::SyscallMiscTests>(
          kContractAddress);

  std::unique_ptr<RuntimeContext> runtime_context_;

  uint64_t nonce_ = 0;
};

// ── Existing tests ──────────────────────────────────────────────────

TEST_F(SyscallMiscTests, InvokeTestGetReturnBeforeReturnBufferSet)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation = client_.make_invocation<
                      "test_get_return_before_return_buffer_set">(UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

TEST_F(SyscallMiscTests, InvokeTestGetReturnAfterFault)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_get_return_after_fault">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  handler.hack_dont_use_test_bypass_allow_reentrance();
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #1: GetMsgSender ────────────────────────────────────────────────

TEST_F(SyscallMiscTests, TestGetMsgSenderRoot)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_get_msg_sender_root">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #2: Invoke (contract not found) ─────────────────────────────────

TEST_F(SyscallMiscTests, TestInvokeContractNotFound)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_invoke_contract_not_found">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #3: Invoke (frame reverted) ─────────────────────────────────────

TEST_F(SyscallMiscTests, TestInvokeFrameReverted)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_invoke_frame_reverted">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  handler.hack_dont_use_test_bypass_allow_reentrance();
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #4: VerifySignature (bad scheme) ────────────────────────────────

TEST_F(SyscallMiscTests, TestVerifySigBadScheme)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_verify_sig_bad_scheme">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #5: VerifySignature (bad key size) ──────────────────────────────

TEST_F(SyscallMiscTests, TestVerifySigBadKeySize)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_verify_sig_bad_key_size">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #6: VerifySignature (bad signature) ─────────────────────────────

TEST_F(SyscallMiscTests, TestVerifySigBadSignature)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_verify_sig_bad_signature">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #7: BytestringGetLen (key not exist) ────────────────────────────

TEST_F(SyscallMiscTests, TestBsGetLenKeyNotExist)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_bs_get_len_key_not_exist">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #8: BytestringGetLen (wrong type) ───────────────────────────────

TEST_F(SyscallMiscTests, TestBsGetLenWrongType)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_bs_get_len_wrong_type">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #9: BytestringGet (key not exist) ───────────────────────────────

TEST_F(SyscallMiscTests, TestBsGetKeyNotExist)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_bs_get_key_not_exist">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #10: BytestringGet (wrong type) ─────────────────────────────────

TEST_F(SyscallMiscTests, TestBsGetWrongType)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_bs_get_wrong_type">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #11: BytestringSet (too long) ───────────────────────────────────

TEST_F(SyscallMiscTests, TestBsSetTooLong)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation = client_.make_invocation<"test_bs_set_too_long">(
                      UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #12: BytestringSet (invalid flags) ──────────────────────────────

TEST_F(SyscallMiscTests, TestBsSetInvalidFlags)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_bs_set_invalid_flags">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #13: NonnegativeIntGet (key not exist) ──────────────────────────

TEST_F(SyscallMiscTests, TestNnintGetKeyNotExist)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_nnint_get_key_not_exist">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #14: NonnegativeIntGet (wrong type) ─────────────────────────────

TEST_F(SyscallMiscTests, TestNnintGetWrongType)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_nnint_get_wrong_type">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #15: NonnegativeIntSetAdd (negative) ────────────────────────────

TEST_F(SyscallMiscTests, TestNnintSetAddNegative)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_nnint_set_add_negative">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #16: NonnegativeIntSetAdd (invalid flags) ───────────────────────

TEST_F(SyscallMiscTests, TestNnintSetAddInvalidFlags)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_nnint_set_add_invalid_flags">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #17: DeleteObject (invalid flags) ───────────────────────────────

TEST_F(SyscallMiscTests, TestDeleteInvalidFlags)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_delete_invalid_flags">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #18: RetrocausalQueueInsert (too large) ─────────────────────────

TEST_F(SyscallMiscTests, TestRcqInsertTooLarge)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_rcq_insert_too_large">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #19: RetrocausalQueueInsert (wrong type) ────────────────────────

TEST_F(SyscallMiscTests, TestRcqInsertWrongType)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_rcq_insert_wrong_type">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #20: RetrocausalQueueConsume (empty) ────────────────────────────

TEST_F(SyscallMiscTests, TestRcqConsumeEmpty)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_rcq_consume_empty">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #21: RetrocausalQueueConsume (wrong type) ───────────────────────

TEST_F(SyscallMiscTests, TestRcqConsumeWrongType)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_rcq_consume_wrong_type">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #22: LogEvent (data too long) ───────────────────────────────────

TEST_F(SyscallMiscTests, TestLogEventDataTooLong)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_log_event_data_too_long">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #23: LogEvent (noncontinuous topics) ────────────────────────────

TEST_F(SyscallMiscTests, TestLogEventNoncontinuousTopics)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_log_event_noncontinuous_topics">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #24: GetAuthClaim (out of bounds) ───────────────────────────────

TEST_F(SyscallMiscTests, TestGetAuthClaimOob)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_get_auth_claim_oob">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #25: SHA256Update (no buffer) ───────────────────────────────────

TEST_F(SyscallMiscTests, TestSha256UpdateNoBuffer)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_sha256_update_no_buffer">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #26: SHA256Update (too many buffers) ────────────────────────────

TEST_F(SyscallMiscTests, TestSha256UpdateTooManyBuffers)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_sha256_update_too_many_buffers">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #27: SHA256Update (buffer too large) ────────────────────────────

TEST_F(SyscallMiscTests, TestSha256UpdateBufferTooLarge)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_sha256_update_buffer_too_large">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #28: SHA256Finalize (no buffer) ─────────────────────────────────

TEST_F(SyscallMiscTests, TestSha256FinalizeNoBuffer)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_sha256_finalize_no_buffer">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #29: SHA256Finalize (double finalize) ───────────────────────────

TEST_F(SyscallMiscTests, TestSha256FinalizeDouble)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_sha256_finalize_double">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #30: GetTransactionStatistics (invalid) ─────────────────────────

TEST_F(SyscallMiscTests, TestGetTxStatsInvalid)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_get_tx_stats_invalid">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #31: ChangePublicKeyRegistration (index oob) ────────────────────

TEST_F(SyscallMiscTests, TestChangePubkeyIndexOob)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_change_pubkey_index_oob">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #32: WriteAddressGasPermissionConfigs (non-initial segment) ─────

TEST_F(SyscallMiscTests, TestGasPermsNonInitialSegment)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_gas_perms_non_initial_segment">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #33: DeployContract (not available) ─────────────────────────────

TEST_F(SyscallMiscTests, TestDeployContractNotAvailable)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_deploy_contract_not_available">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #34: Return buffer too long ─────────────────────────────────────

TEST_F(SyscallMiscTests, TestReturnBufferTooLong)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_return_buffer_too_long">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  handler.hack_dont_use_test_bypass_allow_reentrance();
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #35: DeployContract (already deployed) ──────────────────────────

TEST_F(SyscallMiscTests, TestDeployContractAlreadyDeployed)
{
  using syscall_misc_tests_meta =
      ContractTraitMeta<syscall_misc_tests::SyscallMiscTests>;
  auto contract = syscall_misc_tests_meta::load_wasm();
  auto contract_hash = sha256(*contract);

  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_deploy_contract_already_deployed">(
                  UINT64_MAX, contract_hash),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #36: Invoke (invalid flags) ─────────────────────────────────────

TEST_F(SyscallMiscTests, TestInvokeInvalidFlags)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_invoke_invalid_flags">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #36: Write syscall in read-only frame ───────────────────────────

TEST_F(SyscallMiscTests, TestWriteSyscallInReadOnly)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_write_syscall_in_read_only">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  handler.hack_dont_use_test_bypass_allow_reentrance();
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── Wasm function that returns without calling the Return syscall ────

TEST_F(SyscallMiscTests, InvokeNoReturnSyscall)
{
  // Deploy the same wasm at a second address so we can cross-contract
  // invoke it without reentrance.
  using syscall_misc_tests_meta =
      ContractTraitMeta<syscall_misc_tests::SyscallMiscTests>;
  constexpr ContractAddress kTargetAddress = make_contract_address(1);
  auto contract = syscall_misc_tests_meta::load_wasm();
  write_contract(contract);
  write_deployment(kTargetAddress, contract);

  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_invoke_no_return_syscall">(
                          UINT64_MAX, kTargetAddress),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── Return auth token edge cases ────────────────────────────────────

TEST_F(SyscallMiscTests, TestReturnMisalignedAuthTokens)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_return_misaligned_auth_tokens">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  handler.hack_dont_use_test_bypass_allow_reentrance();
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

TEST_F(SyscallMiscTests, TestReturnRevertWithAuthTokens)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_return_revert_with_auth_tokens">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  handler.hack_dont_use_test_bypass_allow_reentrance();
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

TEST_F(SyscallMiscTests, TestReturnWithEmptyAuthTokens)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_return_with_empty_auth_tokens">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  handler.hack_dont_use_test_bypass_allow_reentrance();
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

TEST_F(SyscallMiscTests, TestReturnWithAuthTokenIds)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_return_with_auth_token_ids">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  handler.hack_dont_use_test_bypass_allow_reentrance();
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

} // namespace hogd
