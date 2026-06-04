// Regression test for issue #2564. See test_ffi_test_helpers.h.
//
// open_mocked_syscall_handler(..., enable_tracing=1) constructs the handler --
// which opens the transaction and registers an open-handler borrow on the DB --
// before enabling tracing. If enable_tracing() throws, the opener now tears the
// handler down through the abort-then-destroy path so the borrow is released
// and the DB stays advanceable/freeable, instead of leaking a handler the
// caller can never close. The throw itself only happens under allocation
// failure (a global allocator hook we deliberately omit), so this test pins the
// externally visible contract on the success path: a traced mocked handler
// opens, closes cleanly, and leaves the DB usable.

#include <gtest/gtest.h>

#include "auth_claim.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {
namespace {

OpenSyscallHandlerResult
open_traced_mocked_handler(void *db, void *rc)
{
  AuthClaim auth_claim_data(AuthClaimType::kSourceAddressAuthData);
  auto serialized_auth_claim_data = x23::serialize(auth_claim_data);
  FFISerializedAuthClaimData ffi_auth_claim_data = {
      .data = serialized_auth_claim_data.data(),
      .len = static_cast<uint32_t>(serialized_auth_claim_data.size()),
  };
  SHA256Hash source_tx_hash{};
  return open_mocked_syscall_handler(db, rc, ffi_auth_claim_data,
                                     source_tx_hash.data(),
                                     /*enable_tracing=*/1);
}

} // namespace

// A tracing-enabled mocked handler opens successfully, does not pin the DB
// after it is closed, and leaves the DB free to advance and be destroyed (issue
// #2564). Before the fix a tracing-setup failure leaked the handler and left
// the DB permanently reporting an open handler.
TEST(TestBindingsTest, TracedMockedHandlerOpenCloseLeavesDbUsable)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);

  auto handler = open_traced_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);

  // While the handler is open the DB is borrowed and must reject advance/free.
  EXPECT_EQ(advance_new_block(db), kTestFFIErrorCodeUnrecoverableError);

  // Closing the handler releases the borrow.
  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);

  // The DB is no longer pinned: it can advance and then be freed.
  EXPECT_EQ(advance_new_block(db), kTestFFIErrorCodeNone);
  EXPECT_EQ(close_runtime_context(rc), kTestFFIErrorCodeNone);
  EXPECT_EQ(free_db(db), kTestFFIErrorCodeNone);
}

} // namespace hogd
