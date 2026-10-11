// Regression test for issue #2667. See test_ffi_test_helpers.h.
//
// open_syscall_handler() and open_mocked_syscall_handler() deserialized their
// argument before borrowing the DB, so a concurrent free_db() was admitted in
// that window and the handler was built on a destroyed DBContext. Both opens
// now borrow at entry. A free_db() that completes before that borrow registers
// remains the caller's to sequence (see test_bindings.h).

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

#include <gtest/gtest.h>

#include "auth_claim.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/serialize.h"

namespace hogd {

namespace {

// Bytes no SignedTransaction or AuthClaim deserializes from: an XDR value
// needs at least a 4-byte discriminant or length, and these are three bytes.
constexpr std::array<uint8_t, 3> kGarbage{0xff, 0xff, 0xff};

SignedTransaction
make_stx(ContractAddress const &addr, uint32_t calldata_bytes)
{
  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 },
             .uniquifier = {.block_expiry_height = INT64_MAX, .nonce = 0}},
      .signatures = {},
  };
  stx.tx.invocation.calldata.resize_uninitialized(calldata_bytes);
  // memset() is __nonnull and calldata.data() is null when empty, which ubsan
  // rejects even for a zero-byte copy (issue #4267).
  if (calldata_bytes > 0) {
    std::memset(stx.tx.invocation.calldata.data(), 0x5a, calldata_bytes);
  }
  return stx;
}

void
free_invoke_result(ForeignTracedInvokeResult const &result)
{
  free_ffi_data_buffer(result.serialized_output);
  free_ffi_data_buffer(result.serialized_trace);
  free_ffi_data_buffer(result.serialized_events);
}

} // namespace

// With a transition in flight the open is refused before deserializing, so
// the refusal comes back rather than the deserialize error the bytes would
// earn. Before the fix the deserialize ran first and this got DeserializeError.
TEST(TestBindingsTest, OpenSyscallHandlerBorrowsBeforeDeserializing)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer free_db_on_exit(
      [db] { EXPECT_EQ(free_db(db), kTestFFIErrorCodeNone); });
  auto *typed_db = static_cast<DBContext *>(db);

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer close_rc_on_exit(
      [rc] { EXPECT_EQ(close_runtime_context(rc), kTestFFIErrorCodeNone); });

  // Not const: the FFI struct takes a mutable pointer.
  auto garbage = kGarbage;
  FFISerializedSignedTransaction ffi_garbage{.data = garbage.data(),
                                             .len = static_cast<uint32_t>(
                                                 garbage.size())};

  // Uncontended the bytes are rejected for what they are.
  auto uncontended = open_syscall_handler(db, rc, ffi_garbage);
  EXPECT_EQ(uncontended.ffi_error_code, kTestFFIErrorCodeDeserializeError);
  EXPECT_EQ(uncontended.syscall_handler, nullptr);
  EXPECT_FALSE(typed_db->has_open_handlers());

  {
    DBContext::LifecycleTransition transition(*typed_db);
    ASSERT_TRUE(transition.held());

    auto refused = open_syscall_handler(db, rc, ffi_garbage);
    EXPECT_EQ(refused.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
    EXPECT_EQ(refused.syscall_handler, nullptr);
    // A refused open borrows nothing.
    EXPECT_FALSE(typed_db->has_open_handlers());
  }

  // The transition is over; a valid open works and leaves the DB clean.
  auto deployment_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_return, nullptr);
  ASSERT_EQ(setup_grant_contract_storage_tokens(db, deployment_addr.data(),
                                                1'000'000),
            kTestFFIErrorCodeNone);
  auto *handler = make_syscall_handler(db, rc, make_stx(deployment_addr, 0));
  ASSERT_NE(handler, nullptr);
  auto result = root_invoke(handler, 0);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  free_invoke_result(result);
  EXPECT_EQ(close_syscall_handler(handler), kTestFFIErrorCodeNone);
  EXPECT_FALSE(typed_db->has_open_handlers());
  EXPECT_EQ(advance_new_block(db, nullptr), kTestFFIErrorCodeNone);
}

// The mocked open has the same shape: an auth claim to deserialize ahead of
// the constructor, and the same entry borrow now ahead of that.
TEST(TestBindingsTest, OpenMockedSyscallHandlerBorrowsBeforeDeserializing)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer free_db_on_exit(
      [db] { EXPECT_EQ(free_db(db), kTestFFIErrorCodeNone); });
  auto *typed_db = static_cast<DBContext *>(db);

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer close_rc_on_exit(
      [rc] { EXPECT_EQ(close_runtime_context(rc), kTestFFIErrorCodeNone); });

  FFISerializedAuthClaimData ffi_garbage{.data = kGarbage.data(),
                                         .len = static_cast<uint32_t>(
                                             kGarbage.size())};
  SHA256Hash source_tx_hash{};

  auto uncontended = open_mocked_syscall_handler(db, rc, ffi_garbage,
                                                 source_tx_hash.data(), 0);
  EXPECT_EQ(uncontended.ffi_error_code, kTestFFIErrorCodeDeserializeError);
  EXPECT_EQ(uncontended.syscall_handler, nullptr);

  {
    DBContext::LifecycleTransition transition(*typed_db);
    ASSERT_TRUE(transition.held());

    auto refused = open_mocked_syscall_handler(db, rc, ffi_garbage,
                                               source_tx_hash.data(), 0);
    EXPECT_EQ(refused.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
    EXPECT_EQ(refused.syscall_handler, nullptr);
    EXPECT_FALSE(typed_db->has_open_handlers());
  }

  auto opened = make_mocked_handler(db, rc);
  ASSERT_EQ(opened.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(opened.syscall_handler, nullptr);
  EXPECT_EQ(close_syscall_handler(opened.syscall_handler),
            kTestFFIErrorCodeNone);
  EXPECT_FALSE(typed_db->has_open_handlers());
  EXPECT_EQ(advance_new_block(db, nullptr), kTestFFIErrorCodeNone);
}

// free_db() while an open with a large transaction is in flight. Waits for the
// DB to report a borrower before freeing, so it never races the handle itself.
TEST(TestBindingsTest, FreeDbIsRefusedWhileOpenSyscallHandlerIsInFlight)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  auto *typed_db = static_cast<DBContext *>(db);

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer close_rc_on_exit(
      [rc] { EXPECT_EQ(close_runtime_context(rc), kTestFFIErrorCodeNone); });

  auto deployment_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_return, nullptr);
  ASSERT_EQ(setup_grant_contract_storage_tokens(db, deployment_addr.data(),
                                                1'000'000),
            kTestFFIErrorCodeNone);

  // Enough calldata that the deserialize takes real time.
  constexpr uint32_t kCalldataBytes = 16U << 20U;
  auto serialized_stx =
      x23::serialize(make_stx(deployment_addr, kCalldataBytes));
  FFISerializedSignedTransaction ffi_stx{.data = serialized_stx.data(),
                                         .len = static_cast<uint32_t>(
                                             serialized_stx.size())};

  OpenSyscallHandlerResult opened{};
  std::thread opener([&] { opened = open_syscall_handler(db, rc, ffi_stx); });
  // Joined on every exit, so a failed assertion below cannot destroy a
  // joinable thread and terminate the process.
  x23::Defer join_opener([&] {
    if (opener.joinable()) {
      opener.join();
    }
  });

  // Wait for the open to be inside the library and holding the DB. With the
  // entry borrow that is nearly immediate; without it the borrow would only
  // appear once the constructor ran, after the deserialize.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!typed_db->has_open_handlers() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(typed_db->has_open_handlers())
      << "open_syscall_handler never registered a borrow";

  // Refused: the open (or by now the handler it built) holds the DB.
  EXPECT_EQ(free_db(db), kTestFFIErrorCodeUnrecoverableError);

  opener.join();
  ASSERT_EQ(opened.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(opened.syscall_handler, nullptr);

  // Still refused while the handler is open; admitted once it is closed.
  EXPECT_EQ(free_db(db), kTestFFIErrorCodeUnrecoverableError);
  EXPECT_EQ(close_syscall_handler(opened.syscall_handler),
            kTestFFIErrorCodeNone);
  EXPECT_FALSE(typed_db->has_open_handlers());
  EXPECT_EQ(free_db(db), kTestFFIErrorCodeNone);
}

} // namespace hogd
