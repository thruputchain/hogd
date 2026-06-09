// Regression test for issue #2544. See test_ffi_test_helpers.h.

#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

// Regression test for #2544: a second try_commit_transaction() on the same
// still-live handle used to re-enter
// SystemCallHandler::try_commit_transaction(), which asserts
// !transaction_committed_or_aborted_ and terminated assertion-enabled builds.
// The exported wrapper now consults its terminal flag (test_bindings.cc:738)
// and rejects the repeat with a structured error.
//
// #2518's DoubleCommitTransactionReturnsErrorInsteadOfAsserting covers the same
// guard via the mocked-frame path (make_mocked_handler + a finished root mock
// frame). This is the symmetric coverage for the real-invocation path: a
// deployed mock contract driven through an honest root_invoke() before the
// duplicate commit, matching the originally reported reproduction.
TEST(TestBindingsTest,
     DoubleTryCommitAfterRealRootInvokeReturnsErrorInsteadOfAsserting)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ContractAddress deployment_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_return, nullptr);
  ASSERT_EQ(setup_grant_contract_storage_tokens(db, deployment_addr.data(),
                                                1'000'000),
            kTestFFIErrorCodeNone);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = deployment_addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 },
             .uniquifier = {.block_expiry_height = INT64_MAX, .nonce = 0}},
      .signatures = {},
  };

  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  ASSERT_NE(syscall_handler, nullptr);

  auto invoke_result = root_invoke(syscall_handler, 0);
  ASSERT_EQ(invoke_result.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(invoke_result.serialized_output);
  free_ffi_data_buffer(invoke_result.serialized_trace);
  free_ffi_data_buffer(invoke_result.serialized_events);

  // The first commit finalizes the transaction.
  auto first_commit = try_commit_transaction(syscall_handler);
  ASSERT_EQ(first_commit.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_EQ(first_commit.result_code,
            std::to_underlying(ExecResult::ExecResultOK));

  // The second commit is rejected with a structured error rather than
  // re-entering the core single-use commit and tripping its assert.
  auto second_commit = try_commit_transaction(syscall_handler);
  EXPECT_EQ(second_commit.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);

  // The handle is still in a clean terminal state and closes normally.
  EXPECT_EQ(close_syscall_handler(syscall_handler), kTestFFIErrorCodeNone);
}

} // namespace hogd
