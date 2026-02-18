#include <cstring>
#include <gtest/gtest.h>
#include <span>
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
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "storage_cost.hh"
#include "transaction.hh"
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

uint32_t
get_event_log_len_unwrapped(void *syscall_handler)
{
  auto result = get_event_log_len(syscall_handler);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  return result.len;
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
        .panic = std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)};
  }

  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)};
  }

  auto r =
      context->to_handler().log_event(nullptr, nullptr, nullptr,
                                      std::as_bytes(std::span("test_event")));
  if (!r.has_value()) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)};
  }

  return ForeignInvokeResult{
      .panic = std::to_underlying(InvokeErrorCode::kInvokeNone)};
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
  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{result.serialized_output.data,
                                         result.serialized_output.len});
  EXPECT_EQ(output.status_code(),
            InvocationFrameStatusCode::kInvocationFrameStatusCodeReturn);

  // Events not available until next block

  auto commit_res = try_commit_transaction(syscall_handler);
  EXPECT_EQ(commit_res.result_code,
            std::to_underlying(ExecResult::ExecResultOK));
  EXPECT_EQ(commit_res.ffi_error_code, kTestFFIErrorCodeNone);

  ASSERT_EQ(advance_new_block(db), kTestFFIErrorCodeNone);

  auto event_log_len = get_event_log_len_unwrapped(syscall_handler);
  EXPECT_GT(event_log_len, 0);

  x23::Buf event_log_buffer;
  // incorrect size to get event data
  EXPECT_NE(get_event_log(syscall_handler, event_log_buffer.data(),
                          event_log_buffer.size()),
            kTestFFIErrorCodeNone);

  event_log_buffer.resize(event_log_len + 1);
  EXPECT_NE(get_event_log(syscall_handler, event_log_buffer.data(),
                          event_log_buffer.size()),
            kTestFFIErrorCodeNone);

  event_log_buffer.resize(event_log_len);
  EXPECT_EQ(get_event_log(syscall_handler, event_log_buffer.data(),
                          event_log_buffer.size()),
            kTestFFIErrorCodeNone);

  TxEventLog event_log;
  x23::deserialize(&event_log, event_log_buffer);
  EXPECT_EQ(event_log.events.size(), 1);
  EXPECT_EQ(event_log.events[0].contract_address, deployment_addr);

  auto const &event = event_log.events[0];
  EXPECT_EQ(event.num_topics, 0);
  EXPECT_EQ(event.serialized_event.size(), 11);
  EXPECT_EQ(std::memcmp(event.serialized_event.data(), "test_event", 11), 0);
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

DecoratedSignature
make_signature(SecretKey sk, Transaction const &tx, uint32_t pk_id)
{
  Signature sig(CryptographicScheme::Ed25519);
  sig.ed25519() = ed25519_sign(sk.ed25519(), x23::serialize(tx));
  return {.public_key_id = pk_id, .signature = sig};
}

constexpr NonceConfig kAnyKeyNonceConfig =
    NonceConfig{.accepted_keys = 0x000F'FFFF, .required_weight = 0};

auto
make_aux_auth_data(ContractAddress addr, x23::BufView serialized_auth_data)
    -> FFISerializedAuthData
{
  FFISerializedAuthData result;
  std::memcpy(result.address, addr.data(), addr.size());
  result.data = serialized_auth_data.data();
  result.len = static_cast<uint32_t>(serialized_auth_data.size());
  return result;
}

auto
make_source_auth_data(x23::BufView serialized_auth_data) -> FFISourceAuthData
{
  return {.data = serialized_auth_data.data(),
          .len = static_cast<uint32_t>(serialized_auth_data.size())};
}

} // namespace

TEST(TestBindingsTest, VerifyAuthWithInvokedAndAuxClaims)
{
  // Test verifying a transaction with:
  // - One auth claim for the invoked address (key 0)
  // - One auth claim for an auxiliary address (key 0)

  // Generate keys for both addresses
  auto [invoked_pk, invoked_sk] = make_key_pair();
  auto [aux_pk, aux_sk] = make_key_pair();

  ContractAddress invoked_addr{1};
  ContractAddress aux_addr{2};

  // Create auth claims
  AuthClaim invoked_claim(AuthClaimType::kInvokedAddressSignature);
  invoked_claim.invoked_address_signature_id() = 0;

  AuthClaim aux_claim(AuthClaimType::kOtherAddressSignature);
  aux_claim.other_address_signature_id().address = aux_addr;
  aux_claim.other_address_signature_id().key_id = 0;

  // Build transaction
  Transaction tx{};
  tx.invocation.invoked_address = invoked_addr;
  tx.invocation.gas_limit = 100'000;
  tx.auth_claims = {invoked_claim, aux_claim};

  // Sign the transaction
  DecoratedSignature invoked_sig = make_signature(invoked_sk, tx, 0);
  DecoratedSignature aux_sig = make_signature(aux_sk, tx, 0);

  SignedTransaction stx{
      .tx = tx,
      .signatures = {DecoratedSignatureList{.list = {invoked_sig}},
                     DecoratedSignatureList{.list = {aux_sig}}}};

  // Create auth data for both addresses
  AddressAuthData invoked_auth_data{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
          new PublicKeyConfig{.key = invoked_pk, .max_weight = Weight{1}})},
      .gas_permissions = {GasPermissionConfig{.limit = 100'000,
                                              .accepted_keys = 1 << 0,
                                              .required_weight = 1}},
      .nonce_config = kAnyKeyNonceConfig};

  AddressAuthData aux_auth_data{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
          new PublicKeyConfig{.key = aux_pk, .max_weight = Weight{1}})},
      .nonce_config = kAnyKeyNonceConfig};

  // Serialize everything
  auto serialized_stx = x23::serialize(stx);
  auto serialized_invoked_auth = x23::serialize(invoked_auth_data);
  auto serialized_aux_auth = x23::serialize(aux_auth_data);

  // Test good verification
  {
    std::vector<FFISerializedAuthData> aux_auth_array = {
        make_aux_auth_data(aux_addr, serialized_aux_auth)};
    uint32_t auth_count = aux_auth_array.size();

    VerifyAuthResult result = verify_signed_transaction_auth(
        {.data = serialized_stx.data(),
         .len = static_cast<uint32_t>(serialized_stx.size())},
        make_source_auth_data(serialized_invoked_auth),
        {.auth_data = aux_auth_array.data(), .count = auth_count});

    EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
    EXPECT_EQ(result.auth_error_code, 0);
  }

  // Test wrong signature
  {
    auto [wrong_pk, wrong_sk] = make_key_pair();
    AddressAuthData wrong_invoked_auth{
        .public_keys = {x23::Ptr<PublicKeyConfig>(
            new PublicKeyConfig{.key = wrong_pk, .max_weight = Weight{1}})},
        .gas_permissions = {GasPermissionConfig{.limit = 100'000,
                                                .accepted_keys = 1 << 0,
                                                .required_weight = 1}},
        .nonce_config = kAnyKeyNonceConfig};

    auto serialized_wrong_auth = x23::serialize(wrong_invoked_auth);

    std::vector<uint8_t> addresses(64);
    std::memcpy(addresses.data(), aux_addr.data(), 32);

    std::vector<FFISerializedAuthData> aux_auth_array = {
        {.data = serialized_aux_auth.data(),
         .len = static_cast<uint32_t>(serialized_aux_auth.size())}};
    uint32_t auth_count = aux_auth_array.size();

    VerifyAuthResult result = verify_signed_transaction_auth(
        {.data = serialized_stx.data(),
         .len = static_cast<uint32_t>(serialized_stx.size())},
        make_source_auth_data(serialized_wrong_auth),
        {.auth_data = aux_auth_array.data(), .count = auth_count});

    EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
    EXPECT_EQ(result.auth_error_code,
              std::to_underlying(
                  AuthVerifyErrorCode::kSignatureVerificationFailed));
  }
}

TEST(TestBindingsTest, VerifyAuthWithInvokedAndAuxClaimsFromDB)
{
  // Test verifying a transaction with:
  // - One auth claim for the invoked address (key 0)
  // - One auth claim for an auxiliary address (key 0)
  // This version reads auth data from the database instead of passing it
  // directly

  // Generate keys for both addresses
  auto [invoked_pk, invoked_sk] = make_key_pair();
  auto [aux_pk, aux_sk] = make_key_pair();

  ContractAddress invoked_addr{1};
  ContractAddress aux_addr{2};
  ContractAddress missing_aux_addr = {3};

  // Create auth claims
  AuthClaim invoked_claim(AuthClaimType::kInvokedAddressSignature);
  invoked_claim.invoked_address_signature_id() = 0;

  AuthClaim aux_claim(AuthClaimType::kOtherAddressSignature);
  aux_claim.other_address_signature_id().address = aux_addr;
  aux_claim.other_address_signature_id().key_id = 0;

  AuthClaim missing_aux_claim(AuthClaimType::kOtherAddressSignature);
  missing_aux_claim.other_address_signature_id().address = missing_aux_addr;
  missing_aux_claim.other_address_signature_id().key_id = 0;

  // Build transaction
  Transaction tx{};
  tx.invocation.invoked_address = invoked_addr;
  tx.invocation.gas_limit = 100'000;
  tx.auth_claims = {invoked_claim, aux_claim};

  // Sign the transaction
  DecoratedSignature invoked_sig = make_signature(invoked_sk, tx, 0);
  DecoratedSignature aux_sig = make_signature(aux_sk, tx, 0);

  SignedTransaction stx{
      .tx = tx,
      .signatures = {DecoratedSignatureList{.list = {invoked_sig}},
                     DecoratedSignatureList{.list = {aux_sig}}}};

  // Create auth data for both addresses
  AddressAuthData invoked_auth_data{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
          new PublicKeyConfig{.key = invoked_pk, .max_weight = Weight{1}})},
      .gas_permissions = {GasPermissionConfig{.limit = 100'000,
                                              .accepted_keys = 1 << 0,
                                              .required_weight = 1}},
      .nonce_config = kAnyKeyNonceConfig};

  AddressAuthData aux_auth_data{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
          new PublicKeyConfig{.key = aux_pk, .max_weight = Weight{1}})},
      .nonce_config = kAnyKeyNonceConfig};

  // Create database and write auth data
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *db_context = reinterpret_cast<DBContext *>(db);
  auto &dao = db_context->get_base_db().get_db();

  // Write auth data to database
  auto write_result =
      dao.write_address_auth_data(invoked_addr, invoked_auth_data);
  ASSERT_TRUE(write_result.has_value())
      << "Failed to write invoked address auth data";

  write_result = dao.write_address_auth_data(aux_addr, aux_auth_data);
  ASSERT_TRUE(write_result.has_value())
      << "Failed to write aux address auth data";

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

  // Test missing auth data
  {
    tx.auth_claims = {invoked_claim, missing_aux_claim};

    // Sign the transaction
    DecoratedSignature invoked_sig = make_signature(invoked_sk, tx, 0);
    DecoratedSignature aux_sig = make_signature(aux_sk, tx, 0);

    SignedTransaction stx{
        .tx = tx,
        .signatures = {DecoratedSignatureList{.list = {invoked_sig}},
                       DecoratedSignatureList{.list = {aux_sig}}}};

    auto serialized_stx = x23::serialize(stx);

    VerifyAuthResultFromDBResult result =
        verify_signed_transaction_auth_from_db(
            db, {.data = serialized_stx.data(),
                 .len = static_cast<uint32_t>(serialized_stx.size())});

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
      .rcq_cost_per_block = ::hogd::to_xdr(int128_t{13}),
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
        .panic = std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)};
  }

  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)};
  }

  ReturnBuffer return_buffer = {0, 1, 2, 3};
  context->to_handler().set_return_buffer(std::move(return_buffer), true);

  return ForeignInvokeResult{
      .panic = std::to_underlying(InvokeErrorCode::kInvokeNone)};
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
  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{result.serialized_output.data,
                                         result.serialized_output.len});
  EXPECT_EQ(output.status_code(),
            InvocationFrameStatusCode::kInvocationFrameStatusCodeAbort);
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
        .panic = std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)};
  }

  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)};
  }

  ReturnBuffer return_buffer = {0, 1, 2, 3};
  context->to_handler().set_return_buffer(std::move(return_buffer), false);

  return ForeignInvokeResult{
      .panic = std::to_underlying(InvokeErrorCode::kInvokeNone)};
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
  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{result.serialized_output.data,
                                         result.serialized_output.len});
  EXPECT_EQ(output.status_code(),
            InvocationFrameStatusCode::kInvocationFrameStatusCodeReturn);
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
        .panic = std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)};
  }

  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)};
  }

  return ForeignInvokeResult{
      .panic = std::to_underlying(InvokeErrorCode::kInvokeDeterministic)};
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
  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{result.serialized_output.data,
                                         result.serialized_output.len});
  EXPECT_EQ(output.status_code(),
            InvocationFrameStatusCode::kInvocationFrameStatusCodeFault);
}

namespace {
// Test mock contract that does a fault
// does not use the foreign context
ForeignInvokeResult
mock_contract_out_of_gas(void * /*unused*/, uint64_t method_number)
{
  if (method_number != 0x1234) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)};
  }

  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)};
  }

  return ForeignInvokeResult{
      .panic = std::to_underlying(InvokeErrorCode::kInvokeOutOfGas)};
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
  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{result.serialized_output.data,
                                         result.serialized_output.len});
  EXPECT_EQ(output.status_code(),
            InvocationFrameStatusCode::kInvocationFrameStatusCodeFault);
}

} // namespace hogd
