// Regression test for issue #2584. See test_ffi_test_helpers.h.
//
// setup_deploy_contract() is noexcept and rejects a contract hash that was
// never created in the DB with the structured
// kTestFFIErrorCodeDeployNonexistentContract. That missing-hash branch logs a
// diagnostic before returning; the log now runs best-effort (#2584), so a
// std::bad_alloc from the logger can no longer cross the noexcept boundary and
// terminate an honest caller. The actual termination only reproduces under
// injected allocation failure (a global allocator hook we deliberately omit,
// matching the existing #2559/#2560 regression tests). This test pins the
// honest success-path invariant: deploying an uncreated hash returns the
// documented structured error and leaves the DB usable.

#include <cstdint>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "types.hh"

namespace hogd {

TEST(TestBindingsTest, DeployUncreatedContractReturnsStructuredError)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  // A 32-byte deployment address and a 32-byte contract hash that has never
  // been created in the DB via setup_create_contract().
  ContractAddress addr{0xAB};
  SHA256Hash uncreated_hash{0xCD};

  EXPECT_EQ(setup_deploy_contract(db, addr.data(), uncreated_hash.data(),
                                  /*contract_size=*/128),
            kTestFFIErrorCodeDeployNonexistentContract);

  // The structured-error path neither terminated nor corrupted state: the DB
  // is still usable afterward.
  EXPECT_EQ(advance_new_block(db), kTestFFIErrorCodeNone);
}

} // namespace hogd
