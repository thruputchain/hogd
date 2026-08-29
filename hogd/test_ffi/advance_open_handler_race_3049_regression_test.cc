// Regression test for issue #3049. See test_ffi_test_helpers.h.
//
// advance_new_block() and free_db() used to sample DBContext's open-handler
// count once and then mutate the state handlers borrow. open_syscall_handler()
// racing that sample could register after the zero observation and bind to a
// StorageSnapshotCache the advance then destroyed, so a later root_invoke()
// read freed memory (and the eventual close reported an unrecoverable error
// against an already-advanced block). Both sides now go through DBContext's
// exclusive lifecycle transition: a transition is admitted only from a state
// with no open handlers, and while one is in flight handler admission is
// refused, so the two can no longer interleave.

#include <atomic>
#include <barrier>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

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

// A transaction against `addr` whose replay nonce is unique per attempt, so
// repeated attempts in one test are not rejected as replays.
SignedTransaction
make_stx(ContractAddress const &addr, std::uint64_t nonce)
{
  return SignedTransaction{
      .tx = {.invocation =
                 {
                     .invoked_address = addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 },
             .uniquifier = {.block_expiry_height = INT64_MAX, .nonce = nonce}},
      .signatures = {},
  };
}

void
free_invoke_result(ForeignTracedInvokeResult const &result)
{
  free_ffi_data_buffer(result.serialized_output);
  free_ffi_data_buffer(result.serialized_trace);
  free_ffi_data_buffer(result.serialized_events);
}

} // namespace

// A lifecycle transition already in flight refuses handler admission, so a
// handler can never bind to snapshot state the transition is replacing. The
// refusal is the structured FFI error, not a crash, and the DB is fully usable
// once the transition ends (issue #3049).
TEST(TestBindingsTest, OpenSyscallHandlerRefusedDuringLifecycleTransition)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  auto *typed_db = static_cast<DBContext *>(db);
  x23::Defer free_db_on_exit(
      [db] { EXPECT_EQ(free_db(db), kTestFFIErrorCodeNone); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer close_rc_on_exit(
      [rc] { EXPECT_EQ(close_runtime_context(rc), kTestFFIErrorCodeNone); });

  auto deployment_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_return, nullptr);
  ASSERT_EQ(setup_grant_contract_storage_tokens(db, deployment_addr.data(),
                                                1'000'000),
            kTestFFIErrorCodeNone);

  auto stx = make_stx(deployment_addr, 0);
  auto serialized_stx = x23::serialize(stx);
  FFISerializedSignedTransaction ffi_stx{.data = serialized_stx.data(),
                                         .len = static_cast<uint32_t>(
                                             serialized_stx.size())};

  {
    DBContext::LifecycleTransition transition(*typed_db);
    ASSERT_TRUE(transition.held());

    auto refused = open_syscall_handler(db, rc, ffi_stx);
    EXPECT_EQ(refused.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
    EXPECT_EQ(refused.syscall_handler, nullptr);
    EXPECT_FALSE(typed_db->has_open_handlers());

    // A second transition is refused for the same reason: the two lifecycle
    // operations are exclusive of each other, not just of open handlers.
    EXPECT_EQ(advance_new_block(db), kTestFFIErrorCodeUnrecoverableError);
  }

  // The transition is over; handler admission works again.
  auto opened = open_syscall_handler(db, rc, ffi_stx);
  ASSERT_EQ(opened.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(opened.syscall_handler, nullptr);
  auto result = root_invoke(opened.syscall_handler, 0);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  free_invoke_result(result);
  EXPECT_EQ(close_syscall_handler(opened.syscall_handler),
            kTestFFIErrorCodeNone);
  EXPECT_FALSE(typed_db->is_lifecycle_poisoned());
}

// Drive advance_new_block() and open_syscall_handler() at each other from two
// threads. At most one of them may be refused -- the loser of the admission
// race -- and a handler that is admitted must hold live snapshot state: its
// root_invoke() and its close must both succeed, and neither may poison the
// DB. Before the fix the pair could both be admitted against the same torn
// state, so the handler read a freed StorageSnapshotCache (an ASan
// heap-use-after-free) or failed its close against an already-advanced block
// (issue #3049).
TEST(TestBindingsTest, ConcurrentAdvanceAndOpenNeverShareTornState)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  auto *typed_db = static_cast<DBContext *>(db);
  x23::Defer free_db_on_exit(
      [db] { EXPECT_EQ(free_db(db), kTestFFIErrorCodeNone); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer close_rc_on_exit(
      [rc] { EXPECT_EQ(close_runtime_context(rc), kTestFFIErrorCodeNone); });

  auto deployment_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_return, nullptr);
  ASSERT_EQ(setup_grant_contract_storage_tokens(db, deployment_addr.data(),
                                                1'000'000),
            kTestFFIErrorCodeNone);

  constexpr std::uint64_t kAttempts = 40;

  for (std::uint64_t attempt = 0; attempt < kAttempts; ++attempt) {
    auto stx = make_stx(deployment_addr, attempt);
    auto serialized_stx = x23::serialize(stx);
    FFISerializedSignedTransaction ffi_stx{.data = serialized_stx.data(),
                                           .len = static_cast<uint32_t>(
                                               serialized_stx.size())};

    std::barrier start(2);
    std::atomic<TestFFIErrorCode> advance_result =
        kTestFFIErrorCodeUnrecoverableError;

    std::thread advance_thread([&] {
      start.arrive_and_wait();
      advance_result.store(advance_new_block(db));
    });

    start.arrive_and_wait();
    auto opened = open_syscall_handler(db, rc, ffi_stx);

    advance_thread.join();

    bool advance_admitted = advance_result.load() == kTestFFIErrorCodeNone;
    bool open_admitted = opened.ffi_error_code == kTestFFIErrorCodeNone;

    // Whoever lost the race was refused because the winner held the DB, so
    // they cannot both lose.
    ASSERT_TRUE(advance_admitted || open_admitted)
        << "attempt " << attempt
        << ": neither the advance nor the handler open was admitted";

    if (!open_admitted) {
      ASSERT_EQ(opened.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
      ASSERT_EQ(opened.syscall_handler, nullptr);
      // A refused open borrows nothing; it succeeds once the advance is done.
      opened = open_syscall_handler(db, rc, ffi_stx);
      ASSERT_EQ(opened.ffi_error_code, kTestFFIErrorCodeNone);
    }
    ASSERT_NE(opened.syscall_handler, nullptr);

    // The handler was admitted, so it borrowed a snapshot no advance can have
    // replaced underneath it: the invocation runs, and the close confirms the
    // transaction against the block the handler actually opened in.
    auto result = root_invoke(opened.syscall_handler, 0);
    EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
    free_invoke_result(result);

    ASSERT_EQ(close_syscall_handler(opened.syscall_handler),
              kTestFFIErrorCodeNone);
    ASSERT_FALSE(typed_db->is_lifecycle_poisoned());
  }
}

} // namespace hogd
