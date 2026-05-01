#include <cassert>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "auth_rpc.hh"
#include "contract.hh"
#include "event.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/wasm/test_load_wasm.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "invoke_flag.hh"
#include "storage_cost.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

namespace {

void *
make_db()
{
  auto db = create_db(nullptr, 0);
  EXPECT_EQ(db.ffi_error_code, kTestFFIErrorCodeNone);
  return db.db;
}

void *
make_runtime_context()
{
  auto rc = open_runtime_context();
  EXPECT_EQ(rc.ffi_error_code, kTestFFIErrorCodeNone);
  return rc.context;
}

void *
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

} // namespace

// Simple tests to make sure that the exported test functions work on at least a
// basic level. This one, for example, catches an error in initializing hogd.
TEST(TestBindingsTest, LazyInitTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  free_db(db);
}

TEST(TestBindingsTest, MissingContractOnDeploymentTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  ContractAddress deployment_addr{};

  Contract contract{};
  auto hash = sha256(contract);

  // the contract size is for deployment costs, which we ignore in this test.
  auto result =
      setup_deploy_contract(db, deployment_addr.data(), hash.data(), 100);
  ASSERT_EQ(result, kTestFFIErrorCodeDeployNonexistentContract);
}

TEST(TestBindingsTest, MissingMockContractTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = 1;

  auto bytes = x23::serialize(contract);

  // the contract size is for deployment costs, which we ignore in this test.
  auto result = setup_create_contract(rc, db, bytes.data(), bytes.size());
  ASSERT_EQ(result, kTestFFIErrorCodeCreateNonexistentMockContract);
}

namespace {
// Test mock contract that creates an event
// In this test, we do not use the method number nor the foreign_context.
ForeignInvokeResult
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
} // namespace

TEST(TestBindingsTest, MockContractCreateEventTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ASSERT_EQ(register_new_contract(rc, 1, mock_contract_create_event, nullptr),
            kTestFFIErrorCodeNone);

  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = 1;
  auto bytes = x23::serialize(contract);
  ASSERT_EQ(setup_create_contract(rc, db, bytes.data(), bytes.size()),
            kTestFFIErrorCodeNone);

  ContractAddress deployment_addr{};
  ASSERT_EQ(setup_deploy_contract(db, deployment_addr.data(),
                                  sha256(contract).data(), bytes.size()),
            kTestFFIErrorCodeNone);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = deployment_addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 }},
      .signatures = {},
  };

  auto serialized_stx = x23::serialize(stx);
  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  x23::Defer d3([syscall_handler] { close_syscall_handler(syscall_handler); });

  auto result = root_invoke(syscall_handler, 0);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_NE(result.serialized_output.data, nullptr);
  x23::Defer d4([&] { free_ffi_data_buffer(result.serialized_output); });
  x23::Defer d5([&] { free_ffi_data_buffer(result.serialized_events); });
  x23::Defer d_tr([&] { free_ffi_data_buffer(result.serialized_trace); });
  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{result.serialized_output.data,
                                         result.serialized_output.len});
  EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Return);

  // Events are returned directly from root_invoke
  ASSERT_NE(result.serialized_events.data, nullptr);
  ASSERT_GT(result.serialized_events.len, 0U);

  x23::Vec<TxEmittedEvent> events;
  x23::deserialize(&events, x23::BufView{result.serialized_events.data,
                                         result.serialized_events.len});
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].contract_address, deployment_addr);
  EXPECT_EQ(events[0].num_topics, 0);
  EXPECT_EQ(events[0].serialized_event.size(), 11);
  EXPECT_EQ(std::memcmp(events[0].serialized_event.data(), "test_event", 11),
            0);

  auto commit_res = try_commit_transaction(syscall_handler);
  EXPECT_EQ(commit_res.result_code,
            std::to_underlying(ExecResult::ExecResultOK));
  EXPECT_EQ(commit_res.ffi_error_code, kTestFFIErrorCodeNone);
}

namespace {
// Mock contract that emits an event then faults
ForeignInvokeResult
mock_contract_event_then_fault(void * /*unused*/, uint64_t method_number)
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

  EXPECT_TRUE(context->to_handler()
                  .log_event(nullptr, nullptr, nullptr,
                             std::as_bytes(std::span("test_event")))
                  .has_value());

  return ForeignInvokeResult{
      .panic = std::to_underlying(InvokeErrorCode::Deterministic)};
}

// Mock contract that emits an event then reverts
ForeignInvokeResult
mock_contract_event_then_revert(void * /*unused*/, uint64_t method_number)
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

  EXPECT_TRUE(context->to_handler()
                  .log_event(nullptr, nullptr, nullptr,
                             std::as_bytes(std::span("test_event")))
                  .has_value());
  context->to_handler().set_return_buffer(ReturnBuffer{0xAB}, true);

  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}
} // namespace

// Verify that root_invoke does NOT return events when the contract faults
TEST(TestBindingsTest, RootInvokeFaultNoEventsTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ASSERT_EQ(register_new_contract(rc, 1, mock_contract_event_then_fault,
                                  nullptr),
            kTestFFIErrorCodeNone);

  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = 1;
  auto bytes = x23::serialize(contract);
  ASSERT_EQ(setup_create_contract(rc, db, bytes.data(), bytes.size()),
            kTestFFIErrorCodeNone);

  ContractAddress deployment_addr{};
  ASSERT_EQ(setup_deploy_contract(db, deployment_addr.data(),
                                  sha256(contract).data(), bytes.size()),
            kTestFFIErrorCodeNone);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = deployment_addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 }},
      .signatures = {},
  };

  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  x23::Defer d3([syscall_handler] { close_syscall_handler(syscall_handler); });

  auto result = root_invoke(syscall_handler, 0);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d4([&] { free_ffi_data_buffer(result.serialized_output); });
  x23::Defer d5([&] { free_ffi_data_buffer(result.serialized_trace); });
  x23::Defer d6([&] { free_ffi_data_buffer(result.serialized_events); });

  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{result.serialized_output.data,
                                         result.serialized_output.len});
  EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Fault);

  // Events must NOT be returned on fault
  EXPECT_EQ(result.serialized_events.data, nullptr);
  EXPECT_EQ(result.serialized_events.len, 0U);
}

// Verify that root_invoke does NOT return events when the contract reverts
TEST(TestBindingsTest, RootInvokeRevertNoEventsTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ASSERT_EQ(register_new_contract(rc, 1, mock_contract_event_then_revert,
                                  nullptr),
            kTestFFIErrorCodeNone);

  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = 1;
  auto bytes = x23::serialize(contract);
  ASSERT_EQ(setup_create_contract(rc, db, bytes.data(), bytes.size()),
            kTestFFIErrorCodeNone);

  ContractAddress deployment_addr{};
  ASSERT_EQ(setup_deploy_contract(db, deployment_addr.data(),
                                  sha256(contract).data(), bytes.size()),
            kTestFFIErrorCodeNone);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = deployment_addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 }},
      .signatures = {},
  };

  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  x23::Defer d3([syscall_handler] { close_syscall_handler(syscall_handler); });

  auto result = root_invoke(syscall_handler, 0);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d4([&] { free_ffi_data_buffer(result.serialized_output); });
  x23::Defer d5([&] { free_ffi_data_buffer(result.serialized_trace); });
  x23::Defer d6([&] { free_ffi_data_buffer(result.serialized_events); });

  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{result.serialized_output.data,
                                         result.serialized_output.len});
  EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Abort);

  // Events must NOT be returned on revert
  EXPECT_EQ(result.serialized_events.data, nullptr);
  EXPECT_EQ(result.serialized_events.len, 0U);

  // Normal workflow: try_commit drains events and handles the abort internally.
  auto commit_res = try_commit_transaction(syscall_handler);
  EXPECT_EQ(commit_res.ffi_error_code, kTestFFIErrorCodeNone);
}

TEST(TestBindingsTest, GrantStorageTokensTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ContractAddress deployment_addr{};

  ASSERT_EQ(setup_grant_contract_storage_tokens(db, deployment_addr.data(),
                                                1000),
            kTestFFIErrorCodeNone);

  ASSERT_EQ(advance_new_block(db), kTestFFIErrorCodeNone);
}

namespace {

std::pair<PublicKey, SecretKey>
make_key_pair()
{
  auto [pk, sk] = ed25519_keygen();
  PublicKey pk_wrapper(CryptographicScheme::Ed25519);
  pk_wrapper.ed25519() = pk;
  SecretKey sk_wrapper(CryptographicScheme::Ed25519);
  sk_wrapper.ed25519() = sk;
  return {pk_wrapper, sk_wrapper};
}

// NOLINTBEGIN(bugprone-unchecked-optional-access)
DecoratedSignature
make_signature(SecretKey sk, Transaction const &tx, uint32_t public_key_id)
{
  Signature sig(CryptographicScheme::Ed25519);
  sig.ed25519() = ed25519_sign(sk.ed25519(), x23::serialize(tx)).value();
  return {.public_key_id = public_key_id, .signature = sig};
}
// NOLINTEND(bugprone-unchecked-optional-access)

auto
make_source_auth_data(x23::BufView serialized_auth_data) -> FFISourceAuthData
{
  return {.data = serialized_auth_data.data(),
          .len = static_cast<uint32_t>(serialized_auth_data.size())};
}

} // namespace

TEST(TestBindingsTest, VerifyAuthWithSourceAddressClaim)
{
  // Test verifying a transaction with a source address signed weight claim

  auto [pk, sk] = make_key_pair();

  ContractAddress invoked_addr{1};

  // Create auth claim
  AuthClaim claim(AuthClaimType::kSourceAddressAuthData);
  claim.source_address_claim().allowed_keys = 1 << 0;
  claim.source_address_claim().required_weight = 1;

  // Build transaction
  Transaction tx{};
  tx.invocation.invoked_address = invoked_addr;
  tx.invocation.gas_limit = 100'000;
  tx.auth_claim = claim;

  // Sign the transaction
  DecoratedSignature sig =
      make_signature(sk, tx, /*public_key_id=*/0);
  SignedTransaction stx{.tx = tx, .signatures = {sig}};

  // Create auth data
  AddressAuthData auth_data{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
          new PublicKeyConfig{.key = pk, .max_weight = Weight{1}})},
      .required_weight = 1};

  // Serialize everything
  auto serialized_stx = x23::serialize(stx);
  auto serialized_auth = x23::serialize(auth_data);

  // Test good verification
  {
    VerifyAuthResult result = verify_signed_transaction_auth(
        {.data = serialized_stx.data(),
         .len = static_cast<uint32_t>(serialized_stx.size())},
        make_source_auth_data(serialized_auth));

    EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
    EXPECT_EQ(result.auth_error_code, 0);
  }

  // Test wrong signature
  {
    auto [wrong_pk, wrong_sk] = make_key_pair();
    AddressAuthData wrong_auth{
        .public_keys = {x23::Ptr<PublicKeyConfig>(
            new PublicKeyConfig{.key = wrong_pk, .max_weight = Weight{1}})},
        .required_weight = 1};

    auto serialized_wrong_auth = x23::serialize(wrong_auth);

    VerifyAuthResult result = verify_signed_transaction_auth(
        {.data = serialized_stx.data(),
         .len = static_cast<uint32_t>(serialized_stx.size())},
        make_source_auth_data(serialized_wrong_auth));

    EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
    EXPECT_EQ(result.auth_error_code,
              std::to_underlying(
                  AuthVerifyErrorCode::kSignatureVerificationFailed));
  }
}

TEST(TestBindingsTest, VerifyAuthFromDB)
{
  // Test verifying a transaction using auth data from the database

  auto [pk, sk] = make_key_pair();
  auto [wrong_pk, wrong_sk] = make_key_pair();

  ContractAddress invoked_addr{1};

  // Create auth claim
  AuthClaim claim(AuthClaimType::kSourceAddressAuthData);
  claim.source_address_claim().allowed_keys = 1 << 0;
  claim.source_address_claim().required_weight = 1;

  // Build transaction
  Transaction tx{};
  tx.invocation.invoked_address = invoked_addr;
  tx.invocation.gas_limit = 100'000;
  tx.auth_claim = claim;

  // Sign the transaction
  DecoratedSignature sig =
      make_signature(sk, tx, /*public_key_id=*/0);
  SignedTransaction stx{.tx = tx, .signatures = {sig}};

  // Create auth data
  AddressAuthData auth_data{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
          new PublicKeyConfig{.key = pk, .max_weight = Weight{1}})},
      .required_weight = 1};

  // Create database and write auth data
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *db_context = reinterpret_cast<DBContext *>(db);
  auto &dao = db_context->get_base_db().get_db();

  // Write auth data to database
  auto write_result = dao.write_address_auth_data(invoked_addr, auth_data);
  ASSERT_TRUE(write_result.has_value())
      << "Failed to write address auth data";

  // Advance block so auth data is available
  ASSERT_EQ(advance_new_block(db), kTestFFIErrorCodeNone);

  // Serialize transaction
  auto serialized_stx = x23::serialize(stx);

  // Test good verification
  {
    VerifyAuthResultFromDBResult result =
        verify_signed_transaction_auth_from_db(
            db, {.data = serialized_stx.data(),
                 .len = static_cast<uint32_t>(serialized_stx.size())});

    EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
    EXPECT_EQ(result.auth_error_code,
              std::to_underlying(AuthErrorCode::kAuthSuccess));
  }

  // Test wrong signature (signed by wrong key)
  {
    DecoratedSignature wrong_sig =
        make_signature(wrong_sk, tx, /*public_key_id=*/0);
    SignedTransaction wrong_stx{.tx = tx, .signatures = {wrong_sig}};

    auto serialized_wrong_stx = x23::serialize(wrong_stx);

    VerifyAuthResultFromDBResult result =
        verify_signed_transaction_auth_from_db(
            db, {.data = serialized_wrong_stx.data(),
                 .len = static_cast<uint32_t>(serialized_wrong_stx.size())});

    EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
    EXPECT_EQ(result.auth_error_code,
              std::to_underlying(AuthErrorCode::kAuthFailed));
  }
}

// Test roundtrip on cost model: create a DB with a non-default cost model,
// retrieve it via get_cost_model, and verify the serialized form matches.
TEST(TestBindingsTest, CostModelRoundtrip)
{
  // Build a non-default cost model with custom values that differ from every
  // default field.
  StorageCostModelXdr custom_model{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = ::hogd::to_xdr(int128_t{42}),
      .bytestring_cost_per_byte = 7,
      .base_contract_deployment_cost = ::hogd::to_xdr(int128_t{100}),
      .contract_deployment_cost_per_byte = ::hogd::to_xdr(int128_t{3}),
      .auth_per_key_cost = ::hogd::to_xdr(int128_t{55}),
      .rcq_cost_per_entry = ::hogd::to_xdr(int128_t{11}),
      .replay_cache_cost_per_entry = ::hogd::to_xdr(int128_t{17}),
      .balance_cost = ::hogd::to_xdr(int128_t{9999}),
  };

  auto serialized_model = x23::serialize(custom_model);

  // Create a DB seeded with the custom cost model.
  auto db_result = create_db(serialized_model.data(),
                             static_cast<uint32_t>(serialized_model.size()));
  ASSERT_EQ(db_result.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(db_result.db, nullptr);
  x23::Defer d([&] { free_db(db_result.db); });

  // Retrieve the cost model back from the DB.
  auto cost_result = get_cost_model(db_result.db);
  ASSERT_EQ(cost_result.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(cost_result.data.data, nullptr);
  ASSERT_GT(cost_result.data.len, 0U);
  x23::Defer d2([&] { free_ffi_data_buffer(cost_result.data); });

  // Deserialize the returned cost model and compare field by field.
  StorageCostModelXdr roundtripped;
  x23::deserialize(&roundtripped,
                   x23::BufView{cost_result.data.data, cost_result.data.len});

  EXPECT_EQ(custom_model, roundtripped);
}

namespace {
// Test mock contract that does a Revert
// does not use the foreign context
ForeignInvokeResult
mock_contract_revert(void * /*unused*/, uint64_t method_number)
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
  context->to_handler().set_return_buffer(std::move(return_buffer), true);

  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}
} // namespace

TEST(TestBindingsTest, MockContractRevertTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ASSERT_EQ(register_new_contract(rc, 1, mock_contract_revert, nullptr),
            kTestFFIErrorCodeNone);

  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = 1;
  auto bytes = x23::serialize(contract);
  ASSERT_EQ(setup_create_contract(rc, db, bytes.data(), bytes.size()),
            kTestFFIErrorCodeNone);

  ContractAddress deployment_addr{};
  ASSERT_EQ(setup_deploy_contract(db, deployment_addr.data(),
                                  sha256(contract).data(), bytes.size()),
            kTestFFIErrorCodeNone);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = deployment_addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 }},
      .signatures = {},
  };

  auto serialized_stx = x23::serialize(stx);
  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  x23::Defer d3([syscall_handler] { close_syscall_handler(syscall_handler); });

  auto result = root_invoke(syscall_handler, 0);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_NE(result.serialized_output.data, nullptr);
  x23::Defer d4([&] { free_ffi_data_buffer(result.serialized_output); });
  x23::Defer d5([&] { free_ffi_data_buffer(result.serialized_trace); });
  x23::Defer d6([&] { free_ffi_data_buffer(result.serialized_events); });
  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{result.serialized_output.data,
                                         result.serialized_output.len});
  EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Abort);
  EXPECT_EQ(output.abort_buffer().size(), 4);
  EXPECT_EQ(output.abort_buffer()[0], 0);
  EXPECT_EQ(output.abort_buffer()[1], 1);
  EXPECT_EQ(output.abort_buffer()[2], 2);
  EXPECT_EQ(output.abort_buffer()[3], 3);
}

namespace {
// Test mock contract that does a Return
// does not use the foreign context
ForeignInvokeResult
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
} // namespace

TEST(TestBindingsTest, MockContractReturnTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ASSERT_EQ(register_new_contract(rc, 1, mock_contract_return, nullptr),
            kTestFFIErrorCodeNone);

  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = 1;
  auto bytes = x23::serialize(contract);
  ASSERT_EQ(setup_create_contract(rc, db, bytes.data(), bytes.size()),
            kTestFFIErrorCodeNone);

  ContractAddress deployment_addr{};
  ASSERT_EQ(setup_deploy_contract(db, deployment_addr.data(),
                                  sha256(contract).data(), bytes.size()),
            kTestFFIErrorCodeNone);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = deployment_addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 }},
      .signatures = {},
  };

  auto serialized_stx = x23::serialize(stx);
  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  x23::Defer d3([syscall_handler] { close_syscall_handler(syscall_handler); });

  auto result = root_invoke(syscall_handler, 0);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_NE(result.serialized_output.data, nullptr);
  x23::Defer d4([&] { free_ffi_data_buffer(result.serialized_output); });
  x23::Defer d5([&] { free_ffi_data_buffer(result.serialized_trace); });
  x23::Defer d6([&] { free_ffi_data_buffer(result.serialized_events); });
  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{result.serialized_output.data,
                                         result.serialized_output.len});
  EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_EQ(output.return_buffer().size(), 4);
  EXPECT_EQ(output.return_buffer()[0], 0);
  EXPECT_EQ(output.return_buffer()[1], 1);
  EXPECT_EQ(output.return_buffer()[2], 2);
  EXPECT_EQ(output.return_buffer()[3], 3);
}

namespace {
// Test mock contract that does a fault
// does not use the foreign context
ForeignInvokeResult
mock_contract_fault(void * /*unused*/, uint64_t method_number)
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

  return ForeignInvokeResult{
      .panic = std::to_underlying(InvokeErrorCode::Deterministic)};
}
} // namespace

TEST(TestBindingsTest, MockContractFaultTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ASSERT_EQ(register_new_contract(rc, 1, mock_contract_fault, nullptr),
            kTestFFIErrorCodeNone);

  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = 1;
  auto bytes = x23::serialize(contract);
  ASSERT_EQ(setup_create_contract(rc, db, bytes.data(), bytes.size()),
            kTestFFIErrorCodeNone);

  ContractAddress deployment_addr{};
  ASSERT_EQ(setup_deploy_contract(db, deployment_addr.data(),
                                  sha256(contract).data(), bytes.size()),
            kTestFFIErrorCodeNone);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = deployment_addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 }},
      .signatures = {},
  };

  auto serialized_stx = x23::serialize(stx);
  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  x23::Defer d3([syscall_handler] { close_syscall_handler(syscall_handler); });

  auto result = root_invoke(syscall_handler, 0);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_NE(result.serialized_output.data, nullptr);
  x23::Defer d4([&] { free_ffi_data_buffer(result.serialized_output); });
  x23::Defer d5([&] { free_ffi_data_buffer(result.serialized_trace); });
  x23::Defer d6([&] { free_ffi_data_buffer(result.serialized_events); });
  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{result.serialized_output.data,
                                         result.serialized_output.len});
  EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Fault);
}

namespace {
// Test mock contract that does a fault
// does not use the foreign context
ForeignInvokeResult
mock_contract_out_of_gas(void * /*unused*/, uint64_t method_number)
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

  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::OutOfGas)};
}
} // namespace

TEST(TestBindingsTest, MockContractOutOfGasTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ASSERT_EQ(register_new_contract(rc, 1, mock_contract_out_of_gas, nullptr),
            kTestFFIErrorCodeNone);

  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = 1;
  auto bytes = x23::serialize(contract);
  ASSERT_EQ(setup_create_contract(rc, db, bytes.data(), bytes.size()),
            kTestFFIErrorCodeNone);

  ContractAddress deployment_addr{};
  ASSERT_EQ(setup_deploy_contract(db, deployment_addr.data(),
                                  sha256(contract).data(), bytes.size()),
            kTestFFIErrorCodeNone);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = deployment_addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 }},
      .signatures = {},
  };

  auto serialized_stx = x23::serialize(stx);
  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  x23::Defer d3([syscall_handler] { close_syscall_handler(syscall_handler); });

  auto result = root_invoke(syscall_handler, 0);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_NE(result.serialized_output.data, nullptr);
  x23::Defer d4([&] { free_ffi_data_buffer(result.serialized_output); });
  x23::Defer d5([&] { free_ffi_data_buffer(result.serialized_trace); });
  x23::Defer d6([&] { free_ffi_data_buffer(result.serialized_events); });
  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{result.serialized_output.data,
                                         result.serialized_output.len});
  EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Fault);
}

TEST(TestBindingsTest, ManualOpenCloseFrameTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  AuthClaimData auth_claim_data = {};
  auto serialized_auth_claim_data = x23::serialize(auth_claim_data);
  FFISerializedAuthClaimData ffi_auth_claim_data = {
      .data = serialized_auth_claim_data.data(),
      .len = static_cast<uint32_t>(serialized_auth_claim_data.size()),
  };

  SHA256Hash source_tx_hash{};

  auto handler = open_mocked_syscall_handler(db, rc, ffi_auth_claim_data,
                                             source_tx_hash.data(), 1);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);

  // can't root invoke on a mocked handler

  auto result = root_invoke(handler.syscall_handler, 0);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
  x23::Defer d_bad_root([&] {
    free_ffi_data_buffer(result.serialized_output);
    free_ffi_data_buffer(result.serialized_trace);
    free_ffi_data_buffer(result.serialized_events);
  });

  Invocation invocation = {
      .invoked_address =
          ContractAddress{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 10000,
      .calldata = {},
  };
  auto serialized_invocation = x23::serialize(invocation);
  FFISerializedInvocation ffi_invocation = {
      .data = serialized_invocation.data(),
      .len = static_cast<uint32_t>(serialized_invocation.size()),
  };
  auto open_res =
      open_mock_invocation_frame(handler.syscall_handler, ffi_invocation, 0);
  EXPECT_EQ(open_res, kTestFFIErrorCodeNone);

  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_NE(finish_res.serialized_output.data, nullptr);
  x23::Defer d5([&] { free_ffi_data_buffer(finish_res.serialized_output); });
  x23::Defer d6([&] { free_ffi_data_buffer(finish_res.serialized_trace); });
  x23::Defer d7([&] { free_ffi_data_buffer(finish_res.serialized_events); });

  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{finish_res.serialized_output.data,
                                         finish_res.serialized_output.len});
  EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_EQ(output.return_buffer().size(), 0);
  EXPECT_EQ(finish_res.gas_consumed, 0);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

namespace {

// std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
// syscall_Invoke_inner(SystemCallHandler &syscall_handler,
//   WrappedGasAmount remaining_gas,
//   const ContractAddress &input_address,
//   uint64_t input_entrypoint_id, GasAmount input_gas_limit,
//   std::span<const std::byte> input_calldata,
//   InvokeFlagSet input_flags,
//   RuntimeMemoryLength &output_return_buffer_len) noexcept

HostFnStatus<SyscallErrorCode>
test_ffi_invoke(const ContractAddress &input_address,
                uint64_t input_entrypoint_id, GasAmount input_gas_limit,
                Calldata const &input_calldata, InvokeFlagSet input_flags,
                std::span<uint32_t const> input_owned_auth_token_ids,
                RuntimeMemoryLength &output_return_buffer_len,
                RuntimeMemoryLength &output_returned_auth_token_len)
{
  auto result = mock_syscall_entrypoint(
      std::to_underlying(SyscallNo::kInvoke),
      reinterpret_cast<uint64_t>(input_address.data()), input_entrypoint_id,
      input_gas_limit, reinterpret_cast<uint64_t>(input_calldata.data()),
      input_calldata.size(), input_flags,
      reinterpret_cast<uint64_t>(input_owned_auth_token_ids.data()),
      input_owned_auth_token_ids.size(),
      reinterpret_cast<uint64_t>(&output_return_buffer_len),
      reinterpret_cast<uint64_t>(&output_returned_auth_token_len));

  if (result.panic != std::to_underlying(HostFnErrorCode::NoneOrRecoverable))
    return HostFnError{static_cast<HostFnErrorCode>(result.panic)};

  return static_cast<SyscallErrorCode>(result.result_code);
}

HostFnStatus<SyscallErrorCode>
test_ffi_get_msg_sender(ContractAddress &output_sender)
{
  auto result =
      mock_syscall_entrypoint(std::to_underlying(SyscallNo::kGetMsgSender),
                              reinterpret_cast<uint64_t>(output_sender.data()),
                              0, 0, 0, 0, 0, 0, 0, 0, 0);

  if (result.panic != std::to_underlying(HostFnErrorCode::NoneOrRecoverable))
    return HostFnError{static_cast<HostFnErrorCode>(result.panic)};

  return static_cast<SyscallErrorCode>(result.result_code);
}

HostFnStatus<SyscallErrorCode>
test_ffi_get_current_address(ContractAddress &output_address)
{
  auto result =
      mock_syscall_entrypoint(std::to_underlying(SyscallNo::kGetCurrentAddress),
                              reinterpret_cast<uint64_t>(output_address.data()),
                              0, 0, 0, 0, 0, 0, 0, 0, 0);

  if (result.panic != std::to_underlying(HostFnErrorCode::NoneOrRecoverable))
    return HostFnError{static_cast<HostFnErrorCode>(result.panic)};

  return static_cast<SyscallErrorCode>(result.result_code);
}

// Helper: set up a mocked syscall handler and return it
OpenSyscallHandlerResult
make_mocked_handler(void *db, void *rc)
{
  AuthClaimData auth_claim_data = {};
  auto serialized_auth_claim_data = x23::serialize(auth_claim_data);
  FFISerializedAuthClaimData ffi_auth_claim_data = {
      .data = serialized_auth_claim_data.data(),
      .len = static_cast<uint32_t>(serialized_auth_claim_data.size()),
  };
  SHA256Hash source_tx_hash{};
  return open_mocked_syscall_handler(db, rc, ffi_auth_claim_data,
                                     source_tx_hash.data(), 1);
}

// Helper: open a mock invocation frame
TestFFIErrorCode
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

// Helper: deploy a mock contract at a specific address and return it
ContractAddress
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

// Mock contract that checks get_msg_sender() and get_current_address()
// foreign_context points to a struct with expected values
struct CallerCheckData {
  ContractAddress expected_sender;
  ContractAddress expected_self;
  bool sender_check_passed = false;
  bool self_check_passed = false;
};

ForeignInvokeResult
mock_contract_check_caller(void *ctx, uint64_t /*method_number*/)
{
  auto *data = reinterpret_cast<CallerCheckData *>(ctx);
  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }

  auto sender = context->to_handler().get_msg_sender();
  data->sender_check_passed =
      sender.has_value() && (*sender == data->expected_sender);

  auto self_addr = context->to_handler().get_current_address();
  data->self_check_passed = (self_addr == data->expected_self);

  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

// Mock contract that just checks get_current_address()
struct SelfCheckData {
  ContractAddress expected_self;
  bool check_passed = false;
};

ForeignInvokeResult
mock_contract_check_self(void *ctx, uint64_t /*method_number*/)
{
  auto *data = reinterpret_cast<SelfCheckData *>(ctx);
  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }

  auto self_addr = context->to_handler().get_current_address();
  data->check_passed = (self_addr == data->expected_self);

  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

// Mock contract that reads calldata and stores it
struct CalldataCheckData {
  std::vector<uint8_t> received_calldata;
};

ForeignInvokeResult
mock_contract_check_calldata(void *ctx, uint64_t /*method_number*/)
{
  assert(ctx != nullptr);
  auto *data = reinterpret_cast<CalldataCheckData *>(ctx);
  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }

  auto const &calldata = context->to_handler().get_calldata();
  data->received_calldata.assign(calldata.begin(), calldata.end());

  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

// Mock contract that sets a return buffer
ForeignInvokeResult
mock_contract_set_return(void * /*unused*/, uint64_t /*method_number*/)
{
  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }

  ReturnBuffer buf = {10, 20, 30, 40};
  context->to_handler().set_return_buffer(std::move(buf), false);

  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

// Mock contract that reverts with an abort buffer
ForeignInvokeResult
mock_contract_revert_with_buffer(void * /*unused*/, uint64_t /*method_number*/)
{
  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }

  ReturnBuffer buf = {0xDE, 0xAD, 0xBE, 0xEF};
  context->to_handler().set_return_buffer(std::move(buf), true);

  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

// Mock contract that invokes another contract (for nested invoke tests)
// foreign_context points to a NestedInvokeData
struct NestedInvokeData {
  ContractAddress target;
  // results from the inner contract
  CallerCheckData *inner_check;
};

ForeignInvokeResult
mock_contract_nested_invoke(void *ctx, uint64_t /*method_number*/)
{
  auto *data = reinterpret_cast<NestedInvokeData *>(ctx);
  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }

  Calldata calldata;
  RuntimeMemoryLength output_return_buffer_len = 0,
                      output_returned_auth_token_len = 0;
  auto res = test_ffi_invoke(data->target, 0x1234, UINT64_MAX, calldata,
                             InvokeFlagSet{}, {}, output_return_buffer_len,
                             output_returned_auth_token_len);
  if (!res.has_value() ||
      res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }
  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

} // namespace

// Verify that when a mock frame at address A invokes a contract at address B,
// the contract sees get_msg_sender() == A and get_current_address() == B.
TEST(TestBindingsTest, MockFrameInvokeContractCallerAddressTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  CallerCheckData check_data;
  auto contract_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_check_caller, &check_data);

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  // Open mock frame at address A
  ContractAddress frame_addr{1, 2,  3,  4,  5,  6,  7,  8,
                             9, 10, 11, 12, 13, 14, 15, 16};
  Invocation invocation = {
      .invoked_address = frame_addr,
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  // Set expected values: sender should be frame_addr (A), self should be
  // contract_addr (B)
  check_data.expected_sender = frame_addr;
  check_data.expected_self = contract_addr;

  // Invoke the contract from within the mock frame
  Calldata calldata;
  RuntimeMemoryLength output_return_buffer_len = 0,
                      output_returned_auth_token_len = 0;

  auto res = test_ffi_invoke(contract_addr, 0x1234, UINT64_MAX, calldata,
                             InvokeFlagSet{}, {}, output_return_buffer_len,
                             output_returned_auth_token_len);

  EXPECT_TRUE(res.has_value());
  EXPECT_EQ(*res, SyscallErrorCode::kSyscallErrorCodeNone);

  EXPECT_TRUE(check_data.sender_check_passed)
      << "get_msg_sender() should return the mock frame's address";
  EXPECT_TRUE(check_data.self_check_passed)
      << "get_current_address() should return the contract's deployment "
         "address";

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d4([&] { free_ffi_data_buffer(finish_res.serialized_output); });
  x23::Defer d5([&] { free_ffi_data_buffer(finish_res.serialized_trace); });
  x23::Defer d6([&] { free_ffi_data_buffer(finish_res.serialized_events); });
  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{finish_res.serialized_output.data,
                                         finish_res.serialized_output.len});
  EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
}

// Verify that a contract invoked from a mock frame sees get_current_address()
// matching its own deployment address.
TEST(TestBindingsTest, MockFrameGetCurrentAddressTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  SelfCheckData check_data;
  auto contract_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_check_self, &check_data);
  check_data.expected_self = contract_addr;

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  ContractAddress frame_addr{42};
  Invocation invocation = {
      .invoked_address = frame_addr,
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  Calldata calldata;
  RuntimeMemoryLength output_return_buffer_len = 0,
                      output_returned_auth_token_len = 0;
  auto res = test_ffi_invoke(contract_addr, 0x1234, UINT64_MAX, calldata,
                             InvokeFlagSet{}, {}, output_return_buffer_len,
                             output_returned_auth_token_len);
  EXPECT_TRUE(res.has_value());
  EXPECT_EQ(*res, SyscallErrorCode::kSyscallErrorCodeNone);

  EXPECT_TRUE(check_data.check_passed)
      << "get_current_address() should match the contract's deployment address";

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish_res.serialized_output);
  free_ffi_data_buffer(finish_res.serialized_trace);
  free_ffi_data_buffer(finish_res.serialized_events);
}

// At the root mock frame, get_msg_sender() should return
// kSyscallErrorCodeIsRootTransaction since there is no parent.
TEST(TestBindingsTest, MockFrameGetMsgSenderAtRootTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  ContractAddress frame_addr{99};
  Invocation invocation = {
      .invoked_address = frame_addr,
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);
  ASSERT_NE(SyscallHandlerContext::get_context(), nullptr);

  // At root frame, get_msg_sender should return an error
  ContractAddress sender;
  auto sender_res = test_ffi_get_msg_sender(sender);
  ASSERT_TRUE(sender_res.has_value());
  EXPECT_EQ(*sender_res, SyscallErrorCode::kSyscallErrorCodeIsRootTransaction);

  // get_current_address should return the frame's address
  ContractAddress current;
  auto current_res = test_ffi_get_current_address(current);
  ASSERT_TRUE(current_res.has_value());
  EXPECT_EQ(*current_res, SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(current, frame_addr);

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish_res.serialized_output);
  free_ffi_data_buffer(finish_res.serialized_trace);
  free_ffi_data_buffer(finish_res.serialized_events);
}

// Gracefully handle syscalls without a context pushed
TEST(TestBindingsTest, MockFrameSyscallWithoutContextTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  ContractAddress frame_addr{99};
  Invocation invocation = {
      .invoked_address = frame_addr,
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(SyscallHandlerContext::get_context(), nullptr);

  ContractAddress addr;
  auto sender_res = test_ffi_get_current_address(addr);
  ASSERT_FALSE(sender_res.has_value());
  ASSERT_EQ(sender_res.error().code(), HostFnErrorCode::Unrecoverable);

  ASSERT_NE(pop_context(), kTestFFIErrorCodeNone);
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish_res.serialized_output);
  free_ffi_data_buffer(finish_res.serialized_trace);
  free_ffi_data_buffer(finish_res.serialized_events);
}

// A -> B -> C: contract C should see get_msg_sender() == B's address
// and get_current_address() == C's address.
TEST(TestBindingsTest, MockFrameNestedInvokeTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  // Deploy contract C (innermost) at address {2} — checks caller/self
  CallerCheckData inner_check;
  auto addr_c = deploy_mock_contract(db, rc, 1, mock_contract_check_caller,
                                     &inner_check, ContractAddress{2});

  // Deploy contract B (middle) at address {1} — invokes contract C
  NestedInvokeData nested_data{.target = addr_c, .inner_check = &inner_check};
  auto addr_b = deploy_mock_contract(db, rc, 2, mock_contract_nested_invoke,
                                     &nested_data, ContractAddress{1});

  // Set expected values for inner contract C
  inner_check.expected_sender = addr_b;
  inner_check.expected_self = addr_c;

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  // Open mock frame at address A
  ContractAddress frame_addr{0xAA};
  Invocation invocation = {
      .invoked_address = frame_addr,
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 10'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  // Invoke contract B from the mock frame (A -> B -> C)
  Calldata calldata;
  RuntimeMemoryLength output_return_buffer_len = 0,
                      output_returned_auth_token_len = 0;
  auto res =
      test_ffi_invoke(addr_b, 0x1234, UINT64_MAX, calldata, InvokeFlagSet{}, {},
                      output_return_buffer_len, output_returned_auth_token_len);
  EXPECT_TRUE(res.has_value());
  EXPECT_EQ(*res, SyscallErrorCode::kSyscallErrorCodeNone);

  // Verify contract C saw B as sender and C as self
  EXPECT_TRUE(inner_check.sender_check_passed)
      << "Nested contract C should see B as msg_sender";
  EXPECT_TRUE(inner_check.self_check_passed)
      << "Nested contract C should see C as current_address";

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish_res.serialized_output);
  free_ffi_data_buffer(finish_res.serialized_trace);
  free_ffi_data_buffer(finish_res.serialized_events);
}

// Verify that calldata passed through invoke() is correctly received by the
// mock contract.
TEST(TestBindingsTest, MockFrameCalldataPassthroughTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  CalldataCheckData check_data;
  auto contract_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_check_calldata,
                           &check_data);

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  ContractAddress frame_addr{0xBB};
  Invocation invocation = {
      .invoked_address = frame_addr,
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  // Create specific calldata
  Calldata calldata = {0x01, 0x02, 0x03, 0xFF};
  RuntimeMemoryLength output_return_buffer_len = 0,
                      output_returned_auth_token_len = 0;
  auto res = test_ffi_invoke(contract_addr, 0x1234, UINT64_MAX, calldata,
                             InvokeFlagSet{}, {}, output_return_buffer_len,
                             output_returned_auth_token_len);
  EXPECT_TRUE(res.has_value());
  EXPECT_EQ(*res, SyscallErrorCode::kSyscallErrorCodeNone);

  // Verify the calldata was received correctly
  ASSERT_EQ(check_data.received_calldata.size(), 4);
  EXPECT_EQ(check_data.received_calldata[0], 0x01);
  EXPECT_EQ(check_data.received_calldata[1], 0x02);
  EXPECT_EQ(check_data.received_calldata[2], 0x03);
  EXPECT_EQ(check_data.received_calldata[3], 0xFF);

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish_res.serialized_output);
  free_ffi_data_buffer(finish_res.serialized_trace);
  free_ffi_data_buffer(finish_res.serialized_events);
}

// Invoke a mock contract that sets a return buffer, then verify the return
// buffer is accessible.
TEST(TestBindingsTest, MockFrameReturnBufferTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  auto contract_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_set_return, nullptr);

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  ContractAddress frame_addr{0xCC};
  Invocation invocation = {
      .invoked_address = frame_addr,
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  Calldata calldata;
  RuntimeMemoryLength output_return_buffer_len = 0,
                      output_returned_auth_token_len = 0;
  auto res = test_ffi_invoke(contract_addr, 0x1234, UINT64_MAX, calldata,
                             InvokeFlagSet{}, {}, output_return_buffer_len,
                             output_returned_auth_token_len);
  EXPECT_TRUE(res.has_value());
  EXPECT_EQ(*res, SyscallErrorCode::kSyscallErrorCodeNone);

  // Check the return buffer is accessible from the parent frame
  auto return_buf_status =
      SyscallHandlerContext::get_context()->to_handler().get_return_buffer();
  ASSERT_TRUE(return_buf_status.has_value());
  ASSERT_TRUE((*return_buf_status).has_value());
  auto const *return_buf = **return_buf_status;
  ASSERT_NE(return_buf, nullptr);
  ASSERT_EQ(return_buf->size(), 4);
  EXPECT_EQ((*return_buf)[0], 10);
  EXPECT_EQ((*return_buf)[1], 20);
  EXPECT_EQ((*return_buf)[2], 30);
  EXPECT_EQ((*return_buf)[3], 40);

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish_res.serialized_output);
  free_ffi_data_buffer(finish_res.serialized_trace);
  free_ffi_data_buffer(finish_res.serialized_events);
}

// Invoke a mock contract that reverts, and verify the parent frame sees
// kSyscallErrorCodeFrameReverted.
TEST(TestBindingsTest, MockFrameRevertedChildTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  auto contract_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_revert_with_buffer,
                           nullptr);

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  ContractAddress frame_addr{0xDD};
  Invocation invocation = {
      .invoked_address = frame_addr,
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  Calldata calldata;
  RuntimeMemoryLength output_return_buffer_len = 0,
                      output_returned_auth_token_len = 0;
  auto res = test_ffi_invoke(contract_addr, 0x1234, UINT64_MAX, calldata,
                             InvokeFlagSet{}, {}, output_return_buffer_len,
                             output_returned_auth_token_len);
  EXPECT_TRUE(res.has_value());
  EXPECT_EQ(*res, SyscallErrorCode::kSyscallErrorCodeFrameReverted);

  // The abort buffer should be accessible as a return buffer
  auto return_buf_status =
      SyscallHandlerContext::get_context()->to_handler().get_return_buffer();
  ASSERT_TRUE(return_buf_status.has_value());
  ASSERT_TRUE((*return_buf_status).has_value());
  auto const *return_buf = **return_buf_status;
  ASSERT_NE(return_buf, nullptr);
  ASSERT_EQ(return_buf->size(), 4);
  EXPECT_EQ((*return_buf)[0], 0xDE);
  EXPECT_EQ((*return_buf)[1], 0xAD);
  EXPECT_EQ((*return_buf)[2], 0xBE);
  EXPECT_EQ((*return_buf)[3], 0xEF);

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d4([&] { free_ffi_data_buffer(finish_res.serialized_output); });
  x23::Defer d5([&] { free_ffi_data_buffer(finish_res.serialized_trace); });
  x23::Defer d6([&] { free_ffi_data_buffer(finish_res.serialized_events); });

  // The outer frame should still return successfully (the revert was handled)
  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{finish_res.serialized_output.data,
                                         finish_res.serialized_output.len});
  EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
}

// Verify that tracing works with nested invokes in mock frames.
// Enable tracing, open mock frame at A, invoke B which invokes C,
// and verify the trace contains the expected structure.
TEST(TestBindingsTest, MockFrameNestedInvokeWithTracingTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  // Deploy contract C (innermost) at address {2} — checks caller/self
  CallerCheckData inner_check;
  auto addr_c = deploy_mock_contract(db, rc, 1, mock_contract_check_caller,
                                     &inner_check, ContractAddress{2});

  // Deploy contract B (middle) at address {1} — invokes contract C
  NestedInvokeData nested_data{.target = addr_c, .inner_check = &inner_check};
  auto addr_b = deploy_mock_contract(db, rc, 2, mock_contract_nested_invoke,
                                     &nested_data, ContractAddress{1});

  inner_check.expected_sender = addr_b;
  inner_check.expected_self = addr_c;

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  ContractAddress frame_addr{0xAA};
  Invocation invocation = {
      .invoked_address = frame_addr,
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 10'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  Calldata calldata;
  RuntimeMemoryLength output_return_buffer_len = 0,
                      output_returned_auth_token_len = 0;
  auto res =
      test_ffi_invoke(addr_b, 0x1234, UINT64_MAX, calldata, InvokeFlagSet{}, {},
                      output_return_buffer_len, output_returned_auth_token_len);
  EXPECT_TRUE(res.has_value());
  EXPECT_EQ(*res, SyscallErrorCode::kSyscallErrorCodeNone);

  EXPECT_TRUE(inner_check.sender_check_passed);
  EXPECT_TRUE(inner_check.self_check_passed);

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d4([&] { free_ffi_data_buffer(finish_res.serialized_output); });

  // Verify trace is present
  ASSERT_NE(finish_res.serialized_trace.data, nullptr);
  ASSERT_GT(finish_res.serialized_trace.len, 0U);
  x23::Defer d5([&] { free_ffi_data_buffer(finish_res.serialized_trace); });
  x23::Defer d6([&] { free_ffi_data_buffer(finish_res.serialized_events); });

  InvocationFrameTrace trace;
  x23::deserialize(&trace, x23::BufView{finish_res.serialized_trace.data,
                                        finish_res.serialized_trace.len});

  // The trace invocation should match what we opened
  EXPECT_EQ(trace.invocation.invoked_address, frame_addr);

  // There should be at least one syscall (the invoke from the mock frame)
  ASSERT_GE(trace.system_calls.size(), 1U);

  // The first syscall should be an Invoke
  auto const &first_syscall = trace.system_calls[0];
  EXPECT_EQ(first_syscall.body.syscall_number(), SyscallNo::kInvoke);

  // The first syscall is the kInvoke from start_mock_invocation_frame,
  // which records the first nested invocation's address.
  auto const &invoke_trace = first_syscall.body.invoke();
  EXPECT_EQ(invoke_trace.inputs.address, addr_b);

  // The invoke should have outputs with a child trace (contract B's trace)
  ASSERT_NE(invoke_trace.outputs, nullptr);
  ASSERT_NE(invoke_trace.outputs->child_trace, nullptr);

  // The child trace is contract B's frame
  auto const &b_trace = *invoke_trace.outputs->child_trace;
  EXPECT_EQ(b_trace.invocation.invoked_address, addr_b);
  ASSERT_GE(b_trace.system_calls.size(), 1U);

  // Contract B's first syscall should be its invoke of contract C
  auto const &b_first_syscall = b_trace.system_calls[0];
  EXPECT_EQ(b_first_syscall.body.syscall_number(), SyscallNo::kInvoke);

  auto const &b_invoke_trace = b_first_syscall.body.invoke();
  EXPECT_EQ(b_invoke_trace.inputs.address, addr_c);

  // Contract C's trace should exist as the grandchild
  ASSERT_NE(b_invoke_trace.outputs, nullptr);
  ASSERT_NE(b_invoke_trace.outputs->child_trace, nullptr);
  auto const &c_trace = *b_invoke_trace.outputs->child_trace;
  EXPECT_EQ(c_trace.invocation.invoked_address, addr_c);

  // The frame output should be Return
  EXPECT_EQ(trace.status.status_code(), InvocationFrameStatusCode::Return);
}

// Test opening and closing mock frames with different error codes
// and verifying the InvocationFrameOutput is correct.
TEST(TestBindingsTest, MockFrameErrorCodeOutputTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  // Test None -> Return with empty return buffer
  {
    auto handler = make_mocked_handler(db, rc);
    ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
    x23::Defer dh([&] { close_syscall_handler(handler.syscall_handler); });

    Invocation invocation = {
        .invoked_address = ContractAddress{1},
        .entrypoint = entrypoint_from_method_id(0x1234),
        .gas_limit = 1'000'000,
        .calldata = {},
    };
    ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
              kTestFFIErrorCodeNone);

    auto finish_res =
        finish_mock_invocation_frame(handler.syscall_handler,
                                     std::to_underlying(InvokeErrorCode::None));
    ASSERT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
    x23::Defer df([&] {
      free_ffi_data_buffer(finish_res.serialized_output);
      free_ffi_data_buffer(finish_res.serialized_trace);
      free_ffi_data_buffer(finish_res.serialized_events);
    });

    InvocationFrameOutput output;
    x23::deserialize(&output, x23::BufView{finish_res.serialized_output.data,
                                           finish_res.serialized_output.len});
    EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
    EXPECT_EQ(output.return_buffer().size(), 0);
  }

  // Test Deterministic -> Fault
  {
    auto handler = make_mocked_handler(db, rc);
    ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
    x23::Defer dh([&] { close_syscall_handler(handler.syscall_handler); });

    Invocation invocation = {
        .invoked_address = ContractAddress{2},
        .entrypoint = entrypoint_from_method_id(0x1234),
        .gas_limit = 1'000'000,
        .calldata = {},
    };
    ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
              kTestFFIErrorCodeNone);

    auto finish_res =
        finish_mock_invocation_frame(handler.syscall_handler,
                                     std::to_underlying(
                                         InvokeErrorCode::Deterministic));
    ASSERT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
    x23::Defer df([&] {
      free_ffi_data_buffer(finish_res.serialized_output);
      free_ffi_data_buffer(finish_res.serialized_trace);
      free_ffi_data_buffer(finish_res.serialized_events);
    });

    InvocationFrameOutput output;
    x23::deserialize(&output, x23::BufView{finish_res.serialized_output.data,
                                           finish_res.serialized_output.len});
    EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Fault);
  }

  // Test OutOfGas -> Fault
  {
    auto handler = make_mocked_handler(db, rc);
    ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
    x23::Defer dh([&] { close_syscall_handler(handler.syscall_handler); });

    Invocation invocation = {
        .invoked_address = ContractAddress{3},
        .entrypoint = entrypoint_from_method_id(0x1234),
        .gas_limit = 1'000'000,
        .calldata = {},
    };
    ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
              kTestFFIErrorCodeNone);

    auto finish_res = finish_mock_invocation_frame(
        handler.syscall_handler, std::to_underlying(InvokeErrorCode::OutOfGas));
    ASSERT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
    x23::Defer df([&] {
      free_ffi_data_buffer(finish_res.serialized_output);
      free_ffi_data_buffer(finish_res.serialized_trace);
      free_ffi_data_buffer(finish_res.serialized_events);
    });

    InvocationFrameOutput output;
    x23::deserialize(&output, x23::BufView{finish_res.serialized_output.data,
                                           finish_res.serialized_output.len});
    EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Fault);
  }
}

// Test that calling set_return_buffer with revert=true inside a mock frame
// produces an Abort status with the correct abort buffer.
TEST(TestBindingsTest, MockFrameManualRevertTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  ContractAddress frame_addr{0xFF};
  Invocation invocation = {
      .invoked_address = frame_addr,
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  // Manually call set_return_buffer with revert=true
  ReturnBuffer revert_buf = {0xCA, 0xFE, 0xBA, 0xBE};
  SyscallHandlerContext::get_context()->to_handler().set_return_buffer(
      std::move(revert_buf), /*revert_frame=*/true);

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  ASSERT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d4([&] {
    free_ffi_data_buffer(finish_res.serialized_output);
    free_ffi_data_buffer(finish_res.serialized_trace);
    free_ffi_data_buffer(finish_res.serialized_events);
  });

  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{finish_res.serialized_output.data,
                                         finish_res.serialized_output.len});
  EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Abort);
  ASSERT_EQ(output.abort_buffer().size(), 4);
  EXPECT_EQ(output.abort_buffer()[0], 0xCA);
  EXPECT_EQ(output.abort_buffer()[1], 0xFE);
  EXPECT_EQ(output.abort_buffer()[2], 0xBA);
  EXPECT_EQ(output.abort_buffer()[3], 0xBE);
}

// Verify that finish_mock_invocation_frame does NOT return events when the
// frame fails with a deterministic error.
TEST(TestBindingsTest, MockFrameFaultNoEventsTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  Invocation invocation = {
      .invoked_address = ContractAddress{0xEE},
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);

  // Emit an event inside the frame
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);
  ASSERT_TRUE(SyscallHandlerContext::get_context()
                  ->to_handler()
                  .log_event(nullptr, nullptr, nullptr,
                             std::as_bytes(std::span("frame_event")))
                  .has_value());
  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);

  // Finish with a deterministic error (fault)
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(
                                       InvokeErrorCode::Deterministic));
  ASSERT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d4([&] {
    free_ffi_data_buffer(finish_res.serialized_output);
    free_ffi_data_buffer(finish_res.serialized_trace);
    free_ffi_data_buffer(finish_res.serialized_events);
  });

  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{finish_res.serialized_output.data,
                                         finish_res.serialized_output.len});
  EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Fault);

  // Events must NOT be returned on fault
  EXPECT_EQ(finish_res.serialized_events.data, nullptr);
  EXPECT_EQ(finish_res.serialized_events.len, 0U);
}

// Verify that finish_mock_invocation_frame does NOT return events when the
// frame is manually reverted via set_return_buffer(_, true).
TEST(TestBindingsTest, MockFrameManualRevertNoEventsTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  Invocation invocation = {
      .invoked_address = ContractAddress{0xDD},
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);

  // Emit an event, then manually revert
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);
  ASSERT_TRUE(SyscallHandlerContext::get_context()
                  ->to_handler()
                  .log_event(nullptr, nullptr, nullptr,
                             std::as_bytes(std::span("frame_event")))
                  .has_value());
  SyscallHandlerContext::get_context()->to_handler().set_return_buffer(
      ReturnBuffer{0xDE, 0xAD}, /*revert_frame=*/true);
  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);

  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  ASSERT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d4([&] {
    free_ffi_data_buffer(finish_res.serialized_output);
    free_ffi_data_buffer(finish_res.serialized_trace);
    free_ffi_data_buffer(finish_res.serialized_events);
  });

  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{finish_res.serialized_output.data,
                                         finish_res.serialized_output.len});
  EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Abort);

  // Events must NOT be returned on manual revert
  EXPECT_EQ(finish_res.serialized_events.data, nullptr);
  EXPECT_EQ(finish_res.serialized_events.len, 0U);

  // Normal workflow: try_commit drains events from last_frame_.
  auto commit_res = try_commit_transaction(handler.syscall_handler);
  EXPECT_EQ(commit_res.ffi_error_code, kTestFFIErrorCodeNone);
}

// Verify that finish_mock_invocation_frame DOES return events on success.
TEST(TestBindingsTest, MockFrameSuccessReturnsEventsTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  ContractAddress frame_addr{0xCC};
  Invocation invocation = {
      .invoked_address = frame_addr,
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);

  // Emit an event inside the frame
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);
  ASSERT_TRUE(SyscallHandlerContext::get_context()
                  ->to_handler()
                  .log_event(nullptr, nullptr, nullptr,
                             std::as_bytes(std::span("frame_event")))
                  .has_value());
  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);

  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  ASSERT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d4([&] { free_ffi_data_buffer(finish_res.serialized_output); });
  x23::Defer d5([&] { free_ffi_data_buffer(finish_res.serialized_events); });
  x23::Defer d_tr([&] { free_ffi_data_buffer(finish_res.serialized_trace); });

  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{finish_res.serialized_output.data,
                                         finish_res.serialized_output.len});
  EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Return);

  // Events MUST be returned on success
  ASSERT_NE(finish_res.serialized_events.data, nullptr);
  ASSERT_GT(finish_res.serialized_events.len, 0);

  x23::Vec<TxEmittedEvent> events;
  x23::deserialize(&events, x23::BufView{finish_res.serialized_events.data,
                                         finish_res.serialized_events.len});
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].contract_address, frame_addr);
  EXPECT_EQ(events[0].serialized_event.size(), 12);
  EXPECT_EQ(std::memcmp(events[0].serialized_event.data(), "frame_event", 12),
            0);

  // Normal workflow: try_commit drains events from last_frame_.
  auto commit_res = try_commit_transaction(handler.syscall_handler);
  EXPECT_EQ(commit_res.ffi_error_code, kTestFFIErrorCodeNone);
}

// Test nested open_mock_invocation_frame calls: A { B {} C {} }
// Verify the trace structure reflects the correct nesting and that
// get_msg_sender() / get_current_address() work correctly at each level.
TEST(TestBindingsTest, MockFrameNestedOpenFramesWithTracingTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  ContractAddress addr_a{0x0A};
  ContractAddress addr_b{0x0B};
  ContractAddress addr_c{0x0C};

  // Open frame A (root)
  Invocation inv_a = {
      .invoked_address = addr_a,
      .entrypoint = entrypoint_from_method_id(0x1111),
      .gas_limit = 10'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, inv_a), kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);
  // At root frame A: get_msg_sender() should return root error
  auto sender_a =
      SyscallHandlerContext::get_context()->to_handler().get_msg_sender();
  EXPECT_FALSE(sender_a.has_value());
  EXPECT_EQ(sender_a.error().code(),
            SyscallErrorCode::kSyscallErrorCodeIsRootTransaction);
  EXPECT_EQ(
      SyscallHandlerContext::get_context()->to_handler().get_current_address(),
      addr_a);

  // Open frame B (child of A)
  Invocation inv_b = {
      .invoked_address = addr_b,
      .entrypoint = entrypoint_from_method_id(0x2222),
      .gas_limit = 5'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, inv_b), kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler),
            kTestFFIErrorCodeUnrecoverableError);
  // Inside B: msg_sender should be A, current_address should be B
  auto sender_b =
      SyscallHandlerContext::get_context()->to_handler().get_msg_sender();
  ASSERT_TRUE(sender_b.has_value());
  EXPECT_EQ(*sender_b, addr_a);
  EXPECT_EQ(
      SyscallHandlerContext::get_context()->to_handler().get_current_address(),
      addr_b);

  // Close frame B with success
  auto finish_b =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  ASSERT_EQ(finish_b.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer db_out([&] { free_ffi_data_buffer(finish_b.serialized_output); });
  x23::Defer db_trace([&] { free_ffi_data_buffer(finish_b.serialized_trace); });
  x23::Defer db_ev([&] { free_ffi_data_buffer(finish_b.serialized_events); });

  {
    InvocationFrameOutput b_output;
    x23::deserialize(&b_output, x23::BufView{finish_b.serialized_output.data,
                                             finish_b.serialized_output.len});
    EXPECT_EQ(b_output.status_code(), InvocationFrameStatusCode::Return);
  }

  // Open frame C (another child of A, after B)
  Invocation inv_c = {
      .invoked_address = addr_c,
      .entrypoint = entrypoint_from_method_id(0x3333),
      .gas_limit = 3'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, inv_c), kTestFFIErrorCodeNone);

  // Inside C: msg_sender should be A, current_address should be C
  auto sender_c =
      SyscallHandlerContext::get_context()->to_handler().get_msg_sender();
  ASSERT_TRUE(sender_c.has_value());
  EXPECT_EQ(*sender_c, addr_a);
  EXPECT_EQ(
      SyscallHandlerContext::get_context()->to_handler().get_current_address(),
      addr_c);

  // Close frame C with a deterministic error (Fault)
  auto finish_c =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(
                                       InvokeErrorCode::Deterministic));
  ASSERT_EQ(finish_c.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer dc_out([&] { free_ffi_data_buffer(finish_c.serialized_output); });
  x23::Defer dc_trace([&] { free_ffi_data_buffer(finish_c.serialized_trace); });
  x23::Defer dc_ev([&] { free_ffi_data_buffer(finish_c.serialized_events); });

  {
    InvocationFrameOutput c_output;
    x23::deserialize(&c_output, x23::BufView{finish_c.serialized_output.data,
                                             finish_c.serialized_output.len});
    EXPECT_EQ(c_output.status_code(), InvocationFrameStatusCode::Fault);
  }

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);

  // Close frame A with success
  auto finish_a =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  ASSERT_EQ(finish_a.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer da_out([&] { free_ffi_data_buffer(finish_a.serialized_output); });
  x23::Defer da_ev([&] { free_ffi_data_buffer(finish_a.serialized_events); });

  {
    InvocationFrameOutput a_output;
    x23::deserialize(&a_output, x23::BufView{finish_a.serialized_output.data,
                                             finish_a.serialized_output.len});
    EXPECT_EQ(a_output.status_code(), InvocationFrameStatusCode::Return);
  }

  // Verify the trace
  ASSERT_NE(finish_a.serialized_trace.data, nullptr);
  ASSERT_GT(finish_a.serialized_trace.len, 0U);
  x23::Defer da_trace([&] { free_ffi_data_buffer(finish_a.serialized_trace); });

  InvocationFrameTrace trace;
  x23::deserialize(&trace, x23::BufView{finish_a.serialized_trace.data,
                                        finish_a.serialized_trace.len});

  // A's trace
  EXPECT_EQ(trace.invocation.invoked_address, addr_a);
  EXPECT_EQ(trace.status.status_code(), InvocationFrameStatusCode::Return);

  // A should have 2 syscall entries: one for B and one for C
  ASSERT_EQ(trace.system_calls.size(), 2U);

  // First syscall: invoke B
  auto const &sc_b = trace.system_calls[0];
  EXPECT_EQ(sc_b.body.syscall_number(), SyscallNo::kInvoke);
  auto const &sc_b_invoke = sc_b.body.invoke();
  EXPECT_EQ(sc_b_invoke.inputs.address, addr_b);
  ASSERT_NE(sc_b_invoke.outputs, nullptr);
  ASSERT_NE(sc_b_invoke.outputs->child_trace, nullptr);

  auto const &b_trace = *sc_b_invoke.outputs->child_trace;
  EXPECT_EQ(b_trace.invocation.invoked_address, addr_b);
  EXPECT_EQ(b_trace.status.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_EQ(b_trace.system_calls.size(), 0U);

  // Second syscall: invoke C
  auto const &sc_c = trace.system_calls[1];
  EXPECT_EQ(sc_c.body.syscall_number(), SyscallNo::kInvoke);
  auto const &sc_c_invoke = sc_c.body.invoke();
  EXPECT_EQ(sc_c_invoke.inputs.address, addr_c);
  ASSERT_NE(sc_c_invoke.outputs, nullptr);
  ASSERT_NE(sc_c_invoke.outputs->child_trace, nullptr);

  auto const &c_trace = *sc_c_invoke.outputs->child_trace;
  EXPECT_EQ(c_trace.invocation.invoked_address, addr_c);
  EXPECT_EQ(c_trace.status.status_code(), InvocationFrameStatusCode::Fault);
  EXPECT_EQ(c_trace.system_calls.size(), 0U);
}

// Regression test: closing a syscall handler with an unfinished outer
// invocation frame should not crash.  Reproduces a panic in
// ~SyscallHandlerContext where mock_runtime_stack_ is non-empty because
// the caller opened two nested frames but only finished the inner one.
//
// Sequence from the original bug report:
//   open_mock_invocation_frame  (frame A)
//   push_context
//   open_mock_invocation_frame  (frame B, nested inside A)
//   pop_context
//   finish_mock_invocation_frame (finishes B, Deterministic)
//   close_syscall_handler        → CHECK(mock_runtime_stack_.empty()) fails
TEST(TestBindingsTest, CloseHandlerWithUnfinishedOuterFrameTest)
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

  ContractAddress addr_a{0x0A};
  ContractAddress addr_b{0x0B};

  // Open frame A (outer)
  Invocation inv_a = {
      .invoked_address = addr_a,
      .entrypoint = entrypoint_from_method_id(0x1111),
      .gas_limit = 10'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, inv_a), kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  // Open frame B (nested inside A)
  Invocation inv_b = {
      .invoked_address = addr_b,
      .entrypoint = entrypoint_from_method_id(0x2222),
      .gas_limit = 5'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, inv_b), kTestFFIErrorCodeNone);

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);

  // Finish only frame B (the inner frame)
  auto finish_b =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(
                                       InvokeErrorCode::Deterministic));
  ASSERT_EQ(finish_b.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish_b.serialized_output);
  free_ffi_data_buffer(finish_b.serialized_trace);
  free_ffi_data_buffer(finish_b.serialized_events);

  // Frame A is still open — close_syscall_handler must not crash.
  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeUnrecoverableError);

  // Now properly close frame A, and tear down the test setup
  auto finish_a =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_a.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish_a.serialized_output);
  free_ffi_data_buffer(finish_a.serialized_trace);
  free_ffi_data_buffer(finish_a.serialized_events);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

namespace {

// Reproduction of SIGABRT from test_ffi trace.
// Pattern: root mock contract -> kInvoke to contract 1 (returns normally) ->
// kInvoke to contract 1 (which kInvoke's contract 2 with gas=1000) ->
// contract 2 calls kSHA256 -> out of gas -> SDK observed a SIGABRT.
//
// Contract 0 (root): deployed, called by root_invoke
//   - kInvoke to contract 1 (first call, returns via kReturn)
//   - kGetReturnedBuffer
//   - kInvoke to contract 1 (second call, which nests into contract 2)
//
// Contract 1 (fibonacci_call analogue):
//   - First call: kReturn (return normally)
//   - Second call: kInvoke to contract 2 with gas_limit=1000
//
// Contract 2 (fibonacci analogue):
//   - kSHA256 (should fail with out of gas)

// MethodIDs passed as arg2 to kInvoke. kInvoke expects the raw MethodID
// (uint64_t), NOT a ContractEntrypointID reinterpreted as uint64_t.
//
// Root cause was an abort_rs bug (fixed in 0.4.2), but leaving the test in
// as it's a reasonable test case anyways.
constexpr uint64_t kTraceMethodFirst = 0xAAAA;
constexpr uint64_t kTraceMethodSecond = 0xBBBB;
constexpr uint64_t kTraceMethodFib = 0xCCCC;

// Contract 2: fibonacci analogue. Calls SHA256, which should run out of gas
// when invoked with very limited gas.
ForeignInvokeResult
trace_contract_fibonacci(void * /*unused*/, uint64_t /*method_number*/)
{
  // Match trace line 68-69: kSHA256 (syscall=0)
  // SHA256 input: 32 bytes; output: 32 bytes
  uint8_t sha_input[32] = {};
  SHA256Hash sha_output = {};
  auto sha_res =
      mock_syscall_entrypoint(std::to_underlying(SyscallNo::kSHA256),
                              reinterpret_cast<uint64_t>(sha_input),
                              sizeof(sha_input),
                              reinterpret_cast<uint64_t>(sha_output.data()),
                              sizeof(sha_output), 0, 0, 0, 0, 0, 0);

  // If SHA256 ran out of gas, panic propagates:
  // trace shows panic=1 (OutOfGas) here
  if (sha_res.panic != 0) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::OutOfGas)};
  }

  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

// Contract 1: fibonacci_call analogue.
// First call (kTraceMethodFirst): just return normally via kReturn.
// Second call (kTraceMethodSecond): kInvoke to contract 2 with gas=1000.
ForeignInvokeResult
trace_contract_fibonacci_call(void *foreign_context, uint64_t method_number)
{
  auto *contract_2_addr = static_cast<ContractAddress *>(foreign_context);

  if (method_number == kTraceMethodFirst) {
    // First call: return normally (matches trace kReturn with Return)
    return ForeignInvokeResult{.panic =
                                   std::to_underlying(InvokeErrorCode::None)};
  }

  if (method_number == kTraceMethodSecond) {
    // Second call: invoke contract 2 with gas_limit=1000
    // Match trace line 65: kInvoke(syscall=5) with arg3=1000
    RuntimeMemoryLength output_return_buffer_len = 0,
                        output_returned_auth_token_len = 0;
    Calldata empty_calldata;
    auto invoke_res =
        test_ffi_invoke(*contract_2_addr, kTraceMethodFib,
                        1000, // gas_limit = 1000, matching the trace
                        empty_calldata, InvokeFlagSet{}, {},
                        output_return_buffer_len,
                        output_returned_auth_token_len);

    // The inner call should have failed (out of gas).
    // Trace shows result_code=-1 here (line 70).
    if (!invoke_res.has_value()) {
      return ForeignInvokeResult{
          .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
    }

    // Even if panic==0, a nonzero result_code indicates error
    return ForeignInvokeResult{.panic =
                                   std::to_underlying(InvokeErrorCode::None)};
  }

  return ForeignInvokeResult{
      .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
}

// Contract 0: root test-script mock contract (deployed).
// - kInvoke to contract 1 (first call, returns normally)
// - kGetReturnedBuffer
// - kInvoke to contract 1 (second call, nests into contract 2)
ForeignInvokeResult
trace_contract_root(void *foreign_context, uint64_t /*method_number*/)
{
  struct RootContext {
    ContractAddress contract_1_addr;
  };
  auto *ctx = static_cast<RootContext *>(foreign_context);

  // Step 1: kInvoke to contract 1, first call (trace lines 40-53)
  {
    RuntimeMemoryLength output_return_buffer_len = 0,
                        output_returned_auth_token_len = 0;
    Calldata empty_calldata;
    auto res = test_ffi_invoke(ctx->contract_1_addr, kTraceMethodFirst,
                               UINT64_MAX, empty_calldata, InvokeFlagSet{}, {},
                               output_return_buffer_len,
                               output_returned_auth_token_len);

    if (!res.has_value()) {
      return ForeignInvokeResult{
          .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
    }
  }

  // Step 2: kGetReturnedBuffer (trace line 54)
  {
    uint8_t return_buf[500] = {}, return_auth_token_buf[500] = {};
    RuntimeMemoryLength return_len = 0, return_auth_token_len = 0;
    mock_syscall_entrypoint(std::to_underlying(SyscallNo::kGetReturnedBuffer),
                            reinterpret_cast<uint64_t>(return_buf),
                            sizeof(return_buf),
                            reinterpret_cast<uint64_t>(&return_len),
                            reinterpret_cast<uint64_t>(return_auth_token_buf),
                            sizeof(return_auth_token_buf),
                            reinterpret_cast<uint64_t>(&return_auth_token_len),
                            0, 0, 0, 0);
  }

  // Step 3: kInvoke to contract 1, second call (trace lines 56-70)
  // This is where contract 1 will call contract 2 with gas=1000
  // and contract 2 will run out of gas -> potential SIGABRT
  {
    RuntimeMemoryLength output_return_buffer_len = 0,
                        output_returned_auth_token_len = 0;
    Calldata empty_calldata;
    auto res = test_ffi_invoke(ctx->contract_1_addr, kTraceMethodSecond,
                               UINT64_MAX, empty_calldata, InvokeFlagSet{}, {},
                               output_return_buffer_len,
                               output_returned_auth_token_len);

    if (!res.has_value()) {
      return ForeignInvokeResult{
          .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
    }
  }

  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

} // namespace

// Reproduction of SIGABRT observed in test_ffi Rust integration test.
// A root mock contract invokes another mock contract which invokes a third
// with limited gas. The innermost contract runs out of gas during SHA256.
//
// This test passed -- it turns out the issue was in abort_rs (fixed in 0.4.2).
TEST(TestBindingsTest, NestedCrossCallOutOfGasFromTrace)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  // Addresses for contracts 1 and 2
  ContractAddress contract_1_addr{};
  contract_1_addr[0] = 1;
  ContractAddress contract_2_addr{};
  contract_2_addr[0] = 2;

  // Root context carries address of contract 1
  struct {
    ContractAddress contract_1_addr;
  } root_context{.contract_1_addr = contract_1_addr};

  // Register mock contracts (matching trace: 3 contracts, same pattern)
  ASSERT_EQ(register_new_contract(rc, 0, trace_contract_root, &root_context),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(register_new_contract(rc, 1, trace_contract_fibonacci_call,
                                  &contract_2_addr),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(register_new_contract(rc, 2, trace_contract_fibonacci, nullptr),
            kTestFFIErrorCodeNone);

  // Create mock contracts in the DB (matching trace: 3 setup_create_contract)
  auto make_mock = [](int32_t id) {
    Contract c(ContractType::kMockTest);
    c.mock_contract_id() = id;
    return c;
  };

  Contract c0 = make_mock(0);
  Contract c1 = make_mock(1);
  Contract c2 = make_mock(2);
  auto bytes0 = x23::serialize(c0);
  auto bytes1 = x23::serialize(c1);
  auto bytes2 = x23::serialize(c2);
  ASSERT_EQ(setup_create_contract(rc, db, bytes0.data(), bytes0.size()),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(setup_create_contract(rc, db, bytes1.data(), bytes1.size()),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(setup_create_contract(rc, db, bytes2.data(), bytes2.size()),
            kTestFFIErrorCodeNone);

  // Deploy all 3 contracts at their addresses
  ContractAddress root_addr{};
  ASSERT_EQ(setup_deploy_contract(db, root_addr.data(), sha256(c0).data(),
                                  bytes0.size()),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(setup_deploy_contract(db, contract_1_addr.data(), sha256(c1).data(),
                                  bytes1.size()),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(setup_deploy_contract(db, contract_2_addr.data(), sha256(c2).data(),
                                  bytes2.size()),
            kTestFFIErrorCodeNone);

  // Grant storage tokens
  ASSERT_EQ(setup_grant_contract_storage_tokens(db, root_addr.data(),
                                                1'000'000'000),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(setup_grant_contract_storage_tokens(db, contract_1_addr.data(),
                                                1'000'000'000),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(setup_grant_contract_storage_tokens(db, contract_2_addr.data(),
                                                1'000'000'000),
            kTestFFIErrorCodeNone);

  ASSERT_EQ(advance_new_block(db), kTestFFIErrorCodeNone);

  // Build signed transaction targeting root contract
  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = root_addr,
                     .entrypoint = entrypoint_from_method_id(0),
                     .gas_limit = UINT64_MAX,
                     .calldata = {},
                 }},
      .signatures = {},
  };

  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  x23::Defer d3([syscall_handler] { close_syscall_handler(syscall_handler); });

  // This is the critical point: root_invoke should NOT SIGABRT.
  // The trace shows it crashed here when the inner SHA256 ran out of gas.
  // That turns out to have been an error in abort_rs (upgrading from 0.4.0 to
  // 0.4.2).
  auto result = root_invoke(syscall_handler, 1 /* enable_tracing */);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(result.serialized_output);
  free_ffi_data_buffer(result.serialized_trace);
  free_ffi_data_buffer(result.serialized_events);
}

// Verify that try_commit_transaction succeeds after the mock invocation path:
// open_mocked_syscall_handler → open_mock_invocation_frame →
// finish_mock_invocation_frame → try_commit_transaction.
TEST(TestBindingsTest, MockFrameTryCommitAfterFinish)
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
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  Invocation invocation = {
      .invoked_address = ContractAddress{},
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = 0,
      .calldata = {},
  };

  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);

  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  ASSERT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer d4([&] { free_ffi_data_buffer(finish_res.serialized_output); });
  x23::Defer d5([&] { free_ffi_data_buffer(finish_res.serialized_events); });
  x23::Defer d6([&] { free_ffi_data_buffer(finish_res.serialized_trace); });

  auto commit_res = try_commit_transaction(handler.syscall_handler);
  EXPECT_EQ(commit_res.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_EQ(commit_res.result_code,
            std::to_underlying(ExecResult::ExecResultOK));
}

// Regression test: concurrent create_db calls from multiple threads used to
// cause use-after-free in PerThread storage (fixed in #1327).
TEST(TestBindingsTest, ConcurrentCreateDB)
{
  constexpr int kThreadCount = 8;
  constexpr int kIterations = 4;

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  for (int t = 0; t < kThreadCount; t++) {
    threads.emplace_back([] {
      for (int i = 0; i < kIterations; i++) {
        auto *db = make_db();
        ASSERT_NE(db, nullptr);
        free_db(db);
      }
    });
  }

  for (auto &th : threads) {
    th.join();
  }
}

// Test that setup_create_contract rejects wasm contracts with memory
// exceeding the allowed limit. See wat/test_large_memory.wat (100 initial
// pages, well above the 1-page limit).
TEST(TestBindingsTest, SetupCreateContractRejectsLargeMemory)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_large_memory.wasm");

  auto bytes = x23::serialize(contract);

  // setup_create_contract should reject this contract because its memory
  // exceeds the allowed limit. Before the fix, this would succeed and the
  // contract would fail later with an Unrecoverable error during instantiation.
  auto result = setup_create_contract(rc, db, bytes.data(), bytes.size());
  EXPECT_EQ(result, kTestFFIErrorCodeContractVerificationFailed);
}

// Test that do_verify_contract returns failure for wasm with large memory.
TEST(TestBindingsTest, VerifyContractRejectsLargeMemory)
{
  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d([rc] { close_runtime_context(rc); });

  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_large_memory.wasm");

  auto bytes = x23::serialize(contract);

  auto result = do_verify_contract(rc, bytes.data(), bytes.size());
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_EQ(result.success, 0);
}


} // namespace hogd
