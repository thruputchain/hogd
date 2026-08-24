#include <gtest/gtest.h>

// Miscellaneous end to end tests on syscall behavior,
// implemented within a contract.
// These tests are just wrappers around the tests in the syscall_misc_tests
// contract.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

#include "built_contracts/syscall_misc_tests.h"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "retrocausal_queue.hh"
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
          TestDB::kHogDb)
  {}

protected:
  void SetUp() override
  {
    using syscall_misc_tests_meta =
        ContractTraitMeta<syscall_misc_tests::SyscallMiscTests>;

    // Load contract
    auto contract = syscall_misc_tests_meta::load_rv32();

    // Deploy contract
    write_contract(contract);
    write_deployment(kContractAddress, contract);

    // Grant storage tokens
    setup_grant_storage_token(kContractAddress, 1'000'000'000);

    runtime_context_ = make_hog_exec_context();
  }

  // Deploys a second copy of the contract, for tests whose entrypoint invokes
  // one of the contract's own `internal_*` helpers: the reentrance guard
  // rejects an invoke of a contract already on the invocation stack, so the
  // helper has to run at a different address.
  void deploy_target()
  {
    using syscall_misc_tests_meta =
        ContractTraitMeta<syscall_misc_tests::SyscallMiscTests>;

    auto contract = syscall_misc_tests_meta::load_rv32();
    write_contract(contract);
    write_deployment(kTargetAddress, contract);
    setup_grant_storage_token(kTargetAddress, 1'000'000'000);
  }

  constexpr static ContractAddress kContractAddress = make_contract_address(0);
  constexpr static ContractAddress kTargetAddress = make_contract_address(1);

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
                      u8"test_get_return_before_return_buffer_set">(UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

TEST_F(SyscallMiscTests, InvokeTestGetReturnAfterFault)
{
  deploy_target();

  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_get_return_after_fault">(
                          UINT64_MAX, kTargetAddress),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #1: GetMsgSender ────────────────────────────────────────────────

TEST_F(SyscallMiscTests, TestGetMsgSenderRoot)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_get_msg_sender_root">(UINT64_MAX),
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
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_invoke_contract_not_found">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #3: Invoke (frame reverted) ─────────────────────────────────────

TEST_F(SyscallMiscTests, TestInvokeFrameReverted)
{
  deploy_target();

  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_invoke_frame_reverted">(
                          UINT64_MAX, kTargetAddress),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #4: VerifySignature (bad scheme) ────────────────────────────────

TEST_F(SyscallMiscTests, TestVerifySigBadScheme)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_verify_sig_bad_scheme">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
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
                      client_.make_invocation<u8"test_verify_sig_bad_key_size">(
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
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_verify_sig_bad_signature">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #7: BytestringGetLen (key not exist) ────────────────────────────

TEST_F(SyscallMiscTests, TestBsGetLenKeyNotExist)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_bs_get_len_key_not_exist">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #8: BytestringGetLen (wrong type) ───────────────────────────────

TEST_F(SyscallMiscTests, TestBsGetLenWrongType)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_bs_get_len_wrong_type">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #9: BytestringGet (key not exist) ───────────────────────────────

TEST_F(SyscallMiscTests, TestBsGetKeyNotExist)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_bs_get_key_not_exist">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
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
              client_.make_invocation<u8"test_bs_get_wrong_type">(UINT64_MAX),
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
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_bs_set_too_long">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #12: BytestringSet (invalid flags) ──────────────────────────────

TEST_F(SyscallMiscTests, TestBsSetInvalidFlags)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_bs_set_invalid_flags">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
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
                      client_.make_invocation<u8"test_nnint_get_key_not_exist">(
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
      Transaction{.invocation =
                      client_.make_invocation<u8"test_nnint_get_wrong_type">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
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
                      client_.make_invocation<u8"test_nnint_set_add_negative">(
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
              client_.make_invocation<u8"test_nnint_set_add_invalid_flags">(
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
      Transaction{.invocation =
                      client_.make_invocation<u8"test_delete_invalid_flags">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #18: RetrocausalQueueInsert (too large) ─────────────────────────

TEST_F(SyscallMiscTests, TestRcqInsertTooLarge)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_rcq_insert_too_large">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #19: RetrocausalQueueInsert (wrong type) ────────────────────────

TEST_F(SyscallMiscTests, TestRcqInsertWrongType)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_rcq_insert_wrong_type">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
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
              client_.make_invocation<u8"test_rcq_consume_empty">(UINT64_MAX),
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
                      client_.make_invocation<u8"test_rcq_consume_wrong_type">(
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
                      client_.make_invocation<u8"test_log_event_data_too_long">(
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
              client_.make_invocation<u8"test_log_event_noncontinuous_topics">(
                  UINT64_MAX),
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
                      client_.make_invocation<u8"test_sha256_update_no_buffer">(
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
              client_.make_invocation<u8"test_sha256_update_too_many_buffers">(
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
              client_.make_invocation<u8"test_sha256_update_buffer_too_large">(
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
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_sha256_finalize_no_buffer">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
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
                      client_.make_invocation<u8"test_sha256_finalize_double">(
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
      Transaction{.invocation =
                      client_.make_invocation<u8"test_get_tx_stats_invalid">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
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
                      client_.make_invocation<u8"test_change_pubkey_index_oob">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
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
              client_.make_invocation<u8"test_deploy_contract_not_available">(
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
  deploy_target();

  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_return_buffer_too_long">(
                          UINT64_MAX, kTargetAddress),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// The Return length gate is strict (> kMaxReturnBufferLen): a child
// returning exactly kMaxReturnBufferLen bytes finishes with frame status
// Return, and the parent retrieves all kMaxReturnBufferLen bytes intact.
// Pins the accept side of the boundary whose reject side is
// TestReturnBufferTooLong.
TEST_F(SyscallMiscTests, TestReturnBufferExactMax)
{
  deploy_target();

  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_return_buffer_exact_max">(
                          UINT64_MAX, kTargetAddress),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #35: DeployContract (already deployed) ──────────────────────────

TEST_F(SyscallMiscTests, TestDeployContractAlreadyDeployed)
{
  using syscall_misc_tests_meta =
      ContractTraitMeta<syscall_misc_tests::SyscallMiscTests>;
  auto contract = syscall_misc_tests_meta::load_rv32();
  auto contract_hash = sha256(*contract);

  auto handler = make_new_system_call_handler(
      Transaction{.invocation = client_.make_invocation<
                      u8"test_deploy_contract_already_deployed">(UINT64_MAX,
                                                                 contract_hash),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #36: Invoke (invalid flags) ─────────────────────────────────────

TEST_F(SyscallMiscTests, TestInvokeInvalidFlags)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_invoke_invalid_flags">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── #36: Write syscall in read-only frame ───────────────────────────

TEST_F(SyscallMiscTests, TestWriteSyscallInReadOnly)
{
  deploy_target();

  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_write_syscall_in_read_only">(
                  UINT64_MAX, kTargetAddress),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── Wasm function that returns without calling the Return syscall ────

TEST_F(SyscallMiscTests, InvokeNoReturnSyscall)
{
  // The contract invokes raw entrypoint 0xFF on the target, whose body
  // returns without calling the Return syscall; on rv32 the contract asserts
  // the child frame faults (the SDK dispatcher ebreaks on wrapper return).
  deploy_target();

  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_invoke_no_return_syscall">(
                  UINT64_MAX, kTargetAddress),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── Return auth token edge cases ────────────────────────────────────

TEST_F(SyscallMiscTests, TestReturnMisalignedAuthTokens)
{
  deploy_target();

  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_return_misaligned_auth_tokens">(
                  UINT64_MAX, kTargetAddress),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

TEST_F(SyscallMiscTests, TestReturnRevertWithAuthTokens)
{
  deploy_target();

  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_return_revert_with_auth_tokens">(
                  UINT64_MAX, kTargetAddress),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

TEST_F(SyscallMiscTests, TestReturnWithEmptyAuthTokens)
{
  deploy_target();

  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_return_with_empty_auth_tokens">(
                  UINT64_MAX, kTargetAddress),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

TEST_F(SyscallMiscTests, TestReturnWithAuthTokenIds)
{
  deploy_target();

  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_return_with_auth_token_ids">(
                  UINT64_MAX, kTargetAddress),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// ── Error-code precedence pins ──────────────────────────────────────

// #37: at the per-tx RCQ observation cap, an empty-queue read reports
// kRetrocausalQueueReadLimitReached (the cap check precedes the empty check)
// while a wrong-type key still reports kInvalidSyscallInput (the item-type
// dispatch precedes the cap check).
TEST_F(SyscallMiscTests, RcqReadCapPrecedence)
{
  {
    auto handler = make_new_system_call_handler(
        Transaction{.invocation =
                        client_.make_invocation<u8"internal_rcq_obs_cap_seed">(
                            UINT64_MAX,
                            uint32_t{kMaxConcurrentRcqReadsPerTx + 1}),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *runtime_context_);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    // Commit so the reading transaction observes the entries as concurrent.
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_rcq_cap_precedence">(
                          UINT64_MAX, uint32_t{kMaxConcurrentRcqReadsPerTx}),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// #38: with no child return buffer, a misaligned restored-auth-token span
// still reports kNoReturnBufferAvailable: the buffer-availability check
// precedes the alignment check (the opposite convention from GetCalldata).
TEST_F(SyscallMiscTests, GetReturnedBufferNoBufferBeatsMisaligned)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation = client_.make_invocation<
                      u8"test_get_returned_buffer_no_buffer_beats_misaligned">(
                      UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// #39: forwarding an unowned auth token on a self-invoke reports
// kInvalidSyscallInput, not kReentrance: token ownership is checked before
// reentrance.
TEST_F(SyscallMiscTests, InvokeUnownedTokenBeatsReentrance)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation = client_.make_invocation<
              u8"test_invoke_unowned_token_beats_reentrance">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// #41: a mutable syscall with an out-of-bounds argument pointer inside a
// read-only frame reports kSyscallErrorCodeFault, not the read-only
// kInvalidSyscallInput: the argument parse check precedes the read-only check.
TEST_F(SyscallMiscTests, ReadOnlyFaultBeatsReadOnlyRejection)
{
  deploy_target();

  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation = client_.make_invocation<
              u8"test_readonly_fault_beats_readonly_rejection">(UINT64_MAX,
                                                                kTargetAddress),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

TEST_F(SyscallMiscTests, OobSyscallArgumentIncrementsSyscallFaultCount)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"internal_bytestring_set_oob_key">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  handler.enable_transaction_stats();
  ASSERT_TRUE(handler.root_invoke().is_root_success());
  auto *stats = handler.get_transaction_stats();
  ASSERT_NE(stats, nullptr);
  EXPECT_EQ(stats->syscall_fault_count, 1);
}

// A hand-written entrypoint record registered outside the #[contract] macro
// (method id 0xFF) has a wrapper that returns without issuing the Return
// syscall: verification passes, but dispatch ebreaks.
TEST_F(SyscallMiscTests,
       RootInvokeOfEntrypointWithoutReturnSyscallIsDeterministicFault)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{
                          .invoked_address = kContractAddress,
                          .entrypoint = entrypoint_from_method_id(0xFF),
                          .gas_limit = 100'000,
                          .calldata = Calldata{},
                      },
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  auto result = handler.root_invoke();
  ASSERT_FALSE(result.is_root_success());
  EXPECT_EQ(result.result_.error().code(), InvokeErrorCode::Deterministic);
}

// #40: reentrance beats resolution. The misc contract runs as the configured
// default wallet at an UNDEPLOYED address, so its own frame address violates
// both the reentrance check and (for a nested invoke, which has no wallet
// fallback) resolution; the self-invoke must report kReentrance.
// NOLINTNEXTLINE(misc-use-internal-linkage)
class SyscallMiscDefaultWalletTest : public ::testing::Test,
                                     public NativeContractTestFramework {
public:
  SyscallMiscDefaultWalletTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb, /*empty_genesis=*/false)
  {}
};

TEST_F(SyscallMiscDefaultWalletTest, ReentranceBeatsUnresolvableSelf)
{
  using syscall_misc_tests_meta =
      ContractTraitMeta<syscall_misc_tests::SyscallMiscTests>;
  auto contract = syscall_misc_tests_meta::load_rv32();

  GenesisConfigurationBuilder builder;
  builder.set_default_wallet(
      NativeContractTestFramework::make_deployed_contract_hash(contract));
  run_genesis_initialization(builder.build(NetworkHash{},
                                           *get_default_cost_model(),
                                           *load_stock_genesis_root_contract()),
                             BlockExecutionMode::kBlockExecutionModeProposal);
  write_contract(contract);

  auto rv32_ctx = std::make_unique<Rv32Context>();
  MultiplexedContext ctx(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kRv32, {rv32_ctx.release(), true}},
      });

  ContractAddress undeployed = make_contract_address(77);
  ContractTraitClient<syscall_misc_tests::SyscallMiscTests> client(undeployed);
  SignedTransaction stx{
      .tx = Transaction{
          .invocation =
              client.make_invocation<u8"test_invoke_self_is_reentrance">(
                  UINT64_MAX),
          .uniquifier = make_new_uniquifier()}};

  auto handler = make_new_system_call_handler(stx, ctx);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

} // namespace hogd
