#pragma once

// Shared helpers for the test_ffi GoogleTest suite.
//
// Each security-regression test lives in its own *_regression_test.cc file so
// that adding a test never edits a shared file (which previously caused
// constant merge conflicts when several test-bearing PRs were in flight). Those
// per-issue files pull the common setup helpers and mock contract callbacks
// from here instead of from test_bindings_test.cc's translation-unit-local
// (anonymous-namespace) copies.

#include <cstdint>
#include <span>
#include <utility>

#include <gtest/gtest.h>

#include "auth_claim.hh"
#include "contract.hh"
#include "event.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/contract_trait.h" // entrypoint_from_method_id
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "invoke_flag.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

inline void *
make_db()
{
  auto db = create_db(nullptr, 0);
  EXPECT_EQ(db.ffi_error_code, kTestFFIErrorCodeNone);
  return db.db;
}

inline void *
make_runtime_context()
{
  auto rc = open_runtime_context();
  EXPECT_EQ(rc.ffi_error_code, kTestFFIErrorCodeNone);
  return rc.context;
}

inline void *
make_syscall_handler(void *db, void *rc, SignedTransaction const &stx)
{
  auto serialized_stx = x23::serialize(stx);
  auto result = open_syscall_handler(
      db, rc,
      FFISerializedSignedTransaction{.data = serialized_stx.data(),
                                     .len = static_cast<uint32_t>(
                                         serialized_stx.size())});
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  return result.syscall_handler;
}

// Set up a mocked syscall handler (tracing enabled) and return it.
inline OpenSyscallHandlerResult
make_mocked_handler(void *db, void *rc)
{
  AuthClaim auth_claim_data(AuthClaimType::kSourceAddressAuthData);
  auto serialized_auth_claim_data = x23::serialize(auth_claim_data);
  FFISerializedAuthClaimData ffi_auth_claim_data = {
      .data = serialized_auth_claim_data.data(),
      .len = static_cast<uint32_t>(serialized_auth_claim_data.size()),
  };
  SHA256Hash source_tx_hash{};
  return open_mocked_syscall_handler(db, rc, ffi_auth_claim_data,
                                     source_tx_hash.data(), 1);
}

// Open a mock invocation frame on a mocked handler.
inline TestFFIErrorCode
open_frame(void *syscall_handler, Invocation const &invocation,
           int32_t flags = 0)
{
  auto serialized = x23::serialize(invocation);
  FFISerializedInvocation ffi = {
      .data = serialized.data(),
      .len = static_cast<uint32_t>(serialized.size()),
  };
  return open_mock_invocation_frame(syscall_handler, ffi, flags);
}

// Register, create, and deploy a kMockTest contract at the given address.
inline ContractAddress
deploy_mock_contract(void *db, void *rc, int32_t mock_id,
                     ForeignEntryPoint entry_point, void *foreign_context,
                     ContractAddress deployment_addr = {})
{
  EXPECT_EQ(register_new_contract(rc, mock_id, entry_point, foreign_context),
            kTestFFIErrorCodeNone);

  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = mock_id;
  auto bytes = x23::serialize(contract);
  EXPECT_EQ(setup_create_contract(rc, db, bytes.data(), bytes.size()),
            kTestFFIErrorCodeNone);

  EXPECT_EQ(setup_deploy_contract(db, deployment_addr.data(),
                                  sha256(contract).data(), bytes.size()),
            kTestFFIErrorCodeNone);
  return deployment_addr;
}

// Mock contract that performs a Return (no events). Ignores the foreign
// context; rejects any method other than 0x1234.
inline ForeignInvokeResult
mock_contract_return(void * /*unused*/, uint64_t method_number)
{
  if (method_number != 0x1234) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }

  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }

  ReturnBuffer return_buffer = {0, 1, 2, 3};
  context->to_handler().set_return_buffer(std::move(return_buffer), false);

  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

// Mock contract that emits one event. Ignores the foreign context; rejects any
// method other than 0x1234.
inline ForeignInvokeResult
mock_contract_create_event(void * /*unused*/, uint64_t method_number)
{
  if (method_number != 0x1234) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }

  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }

  auto r =
      context->to_handler().log_event(nullptr, nullptr, nullptr,
                                      std::as_bytes(std::span("test_event")));
  if (!r.has_value()) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }

  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

} // namespace hogd
