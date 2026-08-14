// Regression test for issue #2693.
//
// A test_ffi mock contract callback (ForeignEntryPoint) is an ordinary C
// function pointer: it is not declared noexcept and is not restricted to
// std::exception throws. MockContract::invoke() calls it directly, and
// MockRuntime::invoke_internal() lets callback exceptions unwind. The internal
// hogd::root_invoke(...) noexcept wrapper caught only std::exception, so a
// non-std::exception throw (e.g. `throw 42;`) reached the noexcept boundary and
// terminated the process instead of returning
// kTestFFIErrorCodeUnrecoverableError.
//
// root_invoke() now has a catch(...) arm that translates any other throw into a
// structured Unrecoverable result, mirroring mock_syscall_entrypoint(). Unlike
// the allocation-failure issues in this cluster, this one is exercised
// directly: the throw lives in the callback we control, so no global allocator
// hook is needed.
//
// MockContract::invoke() has since grown its own catch, so the callback's throw
// is translated at the call rather than unwinding all the way here (#2789);
// root_invoke()'s catch(...) is now the backstop for anything else below it.
// Either way the caller sees kTestFFIErrorCodeUnrecoverableError, which is what
// this test pins.

#include <cstdint>

#include <gtest/gtest.h>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {
namespace {

// Mock contract callback that throws a non-std::exception type. The callback
// signature (ForeignEntryPoint) places no noexcept/std::exception restriction
// on it, so this is a valid -- if hostile -- callback (issue #2693).
ForeignInvokeResult
throws_non_std_exception(void * /*unused*/, uint64_t /*method_number*/)
{
  // NOLINTNEXTLINE(hicpp-exception-baseclass): the point is a non-std throw.
  throw 42;
}

} // namespace

TEST(TestBindingsTest, RootInvokeNonStdCallbackThrowReturnsUnrecoverable)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);

  // Deploy a mock contract whose callback throws a non-std::exception.
  ContractAddress addr{};
  EXPECT_EQ(register_new_contract(rc, 0, throws_non_std_exception, nullptr),
            kTestFFIErrorCodeNone);
  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = 0;
  auto bytes = x23::serialize(contract);
  EXPECT_EQ(setup_create_contract(rc, db, bytes.data(), bytes.size()),
            kTestFFIErrorCodeNone);
  EXPECT_EQ(setup_deploy_contract(db, addr.data(), sha256(contract).data(),
                                  bytes.size()),
            kTestFFIErrorCodeNone);
  EXPECT_EQ(setup_grant_contract_storage_tokens(db, addr.data(), 1'000'000'000),
            kTestFFIErrorCodeNone);
  EXPECT_EQ(advance_new_block(db), kTestFFIErrorCodeNone);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = addr,
                     .entrypoint = entrypoint_from_method_id(0),
                     .gas_limit = UINT64_MAX,
                     .calldata = {},
                 },
             .uniquifier = {.block_expiry_height = INT64_MAX, .nonce = 0}},
      .signatures = {},
  };
  auto *handler = make_syscall_handler(db, rc, stx);
  ASSERT_NE(handler, nullptr);

  // The non-std::exception throw must be translated to a structured error
  // rather than terminating the process (issue #2693).
  auto result = root_invoke(handler, /*enable_tracing=*/0);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
  free_ffi_data_buffer(result.serialized_output);
  free_ffi_data_buffer(result.serialized_trace);
  free_ffi_data_buffer(result.serialized_events);

  // The handler remains closeable after the callback failure (RAII cleanup in
  // MockRuntime::invoke_internal() is preserved).
  EXPECT_EQ(close_syscall_handler(handler), kTestFFIErrorCodeNone);
  EXPECT_EQ(close_runtime_context(rc), kTestFFIErrorCodeNone);
  EXPECT_EQ(free_db(db), kTestFFIErrorCodeNone);
}

} // namespace hogd
