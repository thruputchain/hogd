// Regression test for issue #2518. See test_ffi_test_helpers.h.

#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

// Regression test for #2518: calling abort_transaction() twice on the same
// still-live mocked handler used to re-enter SystemCallHandler::abort_
// transaction(), which asserts !transaction_committed_or_aborted_ and so
// terminated assertion-enabled builds. The wrapper now consults its terminal
// flag and rejects the second abort with a structured error, leaving the
// handle closeable.
TEST(TestBindingsTest, DoubleAbortTransactionReturnsErrorInsteadOfAsserting)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);

  // The first abort finalizes the transaction.
  EXPECT_EQ(abort_transaction(handler.syscall_handler), kTestFFIErrorCodeNone);

  // The second abort is rejected with a structured error rather than tripping
  // the core single-use finalization assert.
  EXPECT_EQ(abort_transaction(handler.syscall_handler),
            kTestFFIErrorCodeUnrecoverableError);

  // The handle is still in a clean terminal state and closes normally.
  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

namespace {

// Drive a mocked handler to a committable state (root mock frame finished) so
// the symmetric commit-path tests below can call try_commit_transaction().
void
finish_root_mock_frame(void *handler)
{
  Invocation invocation = {
      .invoked_address = ContractAddress{},
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = 0,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler, invocation), kTestFFIErrorCodeNone);
  auto finish_res =
      finish_mock_invocation_frame(handler,
                                   std::to_underlying(InvokeErrorCode::None));
  ASSERT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish_res.serialized_output);
  free_ffi_data_buffer(finish_res.serialized_events);
  free_ffi_data_buffer(finish_res.serialized_trace);
}

} // namespace

// Symmetric coverage for #2518 (greptile review): try_commit_transaction()
// re-entered the same single-use core method, which also asserts
// !transaction_committed_or_aborted_. A repeat commit is now rejected with a
// structured error instead of crashing assertion-enabled builds.
TEST(TestBindingsTest, DoubleCommitTransactionReturnsErrorInsteadOfAsserting)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);

  finish_root_mock_frame(handler.syscall_handler);

  // The first commit finalizes the transaction.
  auto first = try_commit_transaction(handler.syscall_handler);
  free_ffi_data_buffer(first.serialized_conflict);
  EXPECT_EQ(first.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_EQ(first.result_code, std::to_underlying(ExecResult::ExecResultOK));

  // The second commit is rejected rather than re-entering the core assert.
  auto second = try_commit_transaction(handler.syscall_handler);
  free_ffi_data_buffer(second.serialized_conflict);
  EXPECT_EQ(second.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

// Symmetric coverage for #2518 (greptile review): committing after a prior
// abort would also re-enter the single-use core commit and assert. It is now
// rejected with a structured error.
TEST(TestBindingsTest, CommitAfterAbortReturnsErrorInsteadOfAsserting)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);

  finish_root_mock_frame(handler.syscall_handler);

  // Abort finalizes the transaction.
  EXPECT_EQ(abort_transaction(handler.syscall_handler), kTestFFIErrorCodeNone);

  // A commit on the now-terminal handle is rejected, not asserted.
  auto commit = try_commit_transaction(handler.syscall_handler);
  free_ffi_data_buffer(commit.serialized_conflict);
  EXPECT_EQ(commit.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

} // namespace hogd
