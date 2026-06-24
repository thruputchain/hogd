// Regression test for the test_ffi setup-path noexcept-boundary cluster:
// issues #2641, #2642, #2765, and #2766. See test_ffi_test_helpers.h.
//
// These exported setup entry points are noexcept, but each had an allocation
// that could throw std::bad_alloc and cross the noexcept boundary on an honest
// caller, terminating the process instead of returning a structured FFI error:
//
//   * #2641: create_db() runs a best-effort start trace before make_new_db().
//     On a fresh process that lazily initializes x23::DebugFd, whose noexcept
//     make_fd() assigned env_val = get_debug_env(...) (a std::string alloc)
//     with no catch. make_fd() now guards the env lookup and disables the fd on
//     failure, so create_db() stays best-effort.
//   * #2765: setup_deploy_contract() read the stored contract row
//     (read_wasm_code() -> RAMDB make_byte_buffer() allocates) before any
//     catch.
//   * #2642/#2766: setup_deploy_contract() then decoded the stored bytes in a
//     block that caught only x23::XdrException, so a std::bad_alloc from the
//     opaque-vector resize escaped. The read and decode are now wrapped in one
//     std::exception boundary that returns kTestFFIErrorCodeUnrecoverableError.
//
// The allocation-failure branches can only be forced with a global allocator
// hook, which (matching the other setup/trace regression tests) we deliberately
// do not install. These tests pin the success-path invariant: the first FFI
// call runs the start trace and creates a DB, and a valid created contract can
// be read back, deserialized, and deployed without crashing.

#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

// #2641: make_db() is the first FFI call in this process-or-test, so it drives
// create_db()'s start trace through x23::DebugFd::make_fd() on first use. The
// guarded env lookup must not crash; the DB opens and frees cleanly.
TEST(TestBindingsTest, CreateDbFirstCallRunsStartTraceWithoutCrashing)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  EXPECT_EQ(free_db(db), kTestFFIErrorCodeNone);
}

// #2642/#2765/#2766: setup_deploy_contract() reads the stored contract row and
// deserializes it inside one std::exception boundary. On the success path the
// stored contract created by setup_create_contract() is read back,
// deserialized, and deployed, returning kTestFFIErrorCodeNone.
TEST(TestBindingsTest,
     SetupDeployReadsAndDeserializesStoredContractWithoutCrashing)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  // register + create the contract whose stored bytes deploy reads back.
  ForeignEntryPoint entry = [](void *, uint64_t) {
    return ForeignInvokeResult{.panic =
                                   std::to_underlying(InvokeErrorCode::None)};
  };
  ASSERT_EQ(register_new_contract(rc, 0, entry, nullptr),
            kTestFFIErrorCodeNone);
  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = 0;
  auto bytes = x23::serialize(contract);
  ASSERT_EQ(setup_create_contract(rc, db, bytes.data(), bytes.size()),
            kTestFFIErrorCodeNone);

  ContractAddress addr{0x42};
  EXPECT_EQ(setup_deploy_contract(db, addr.data(), sha256(contract).data(),
                                  bytes.size()),
            kTestFFIErrorCodeNone);

  // Deploying a hash that was never created still returns the structured
  // not-found code (and never crosses the noexcept boundary).
  ContractAddress other_addr{0x43};
  SHA256Hash missing_hash{};
  missing_hash[0] = 0xFF;
  EXPECT_EQ(setup_deploy_contract(db, other_addr.data(), missing_hash.data(),
                                  bytes.size()),
            kTestFFIErrorCodeDeployNonexistentContract);
}

} // namespace hogd
