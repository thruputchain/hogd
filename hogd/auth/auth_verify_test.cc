#include <gtest/gtest.h>

#include <utility>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "auth_rpc.hh"
#include "auth_server_test_framework.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

namespace {

std::pair<PublicKey, SecretKey>
make_key_pair()
{
  auto [pk, sk] = ed25519_keygen();
  PublicKey key(CryptographicScheme::Ed25519);
  key.ed25519() = pk;
  SecretKey secret_key(CryptographicScheme::Ed25519);
  secret_key.ed25519() = sk;
  return {key, secret_key};
}

constexpr AuthClaim
make_self_individual_key_claim(uint8_t id)
{
  AuthClaim claim(AuthClaimType::kSourceAddressAuthData);
  claim.source_address_claim().allowed_keys = static_cast<uint32_t>(1) << id;
  claim.source_address_claim().required_weight = 1;
  return claim;
}

// NOLINTBEGIN(bugprone-unchecked-optional-access)
constexpr DecoratedSignature
make_signature(SecretKey sk, Transaction const &tx, uint32_t public_key_id)
{
  DecoratedSignature signature;
  signature.public_key_id = public_key_id;
  signature.signature = Signature(CryptographicScheme::Ed25519);
  signature.signature.ed25519() =
      ed25519_sign(sk.ed25519(), x23::serialize(tx)).value();
  return signature;
}
// NOLINTEND(bugprone-unchecked-optional-access)

void
add_pk_to_auth_data(AddressAuthData &auth_data, uint8_t id, PublicKey const &pk)
{
  auth_data.public_keys[id] = x23::Ptr<PublicKeyConfig>(
      new PublicKeyConfig{.key = pk, .max_weight = 100});
}

} // namespace

class AuthVerifyTest : public ::testing::Test, public AuthServerTestFramework {
protected:
private:
};

TEST_F(AuthVerifyTest, VerifyTransaction)
{
  ContractAddress address = make_test_address(8);

  AddressAuthData auth_data{};
  auto [pk, sk] = make_key_pair();
  add_pk_to_auth_data(auth_data, 0, pk);

  auth_data.required_weight = 1;

  mock_db().increment_height(); // height 1
  mock_db().set(address, auth_data);
  mock_db().compute_key_changes_for_current_height();

  auth_server().register_auth_data_updates(1,
                                           mock_db().get_key_changes_at_height(
                                               1))();

  Transaction tx{};
  tx.invocation.invoked_address = address;
  tx.auth_claim = make_self_individual_key_claim(0);

  SignedTransaction stx{};
  stx.tx = tx;
  stx.signatures.push_back(make_signature(sk, tx, /*public_key_id=*/0));

  x23::Vec<SignedTransaction> stx_vec{stx};
  x23::span<SignedTransaction> stx_span(stx_vec);
  auto result = auth_server().verify_transactions(1, stx_span)();
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0], AuthErrorCode::kAuthSuccess);
}

TEST_F(AuthVerifyTest, VerifyMultipleTransaction)
{
  ContractAddress address1 = make_test_address(8);
  ContractAddress address2 = make_test_address(9);

  AddressAuthData auth_data1{};
  AddressAuthData auth_data2{};

  auto [pk1, sk1] = make_key_pair();
  auto [pk2, sk2] = make_key_pair();

  add_pk_to_auth_data(auth_data1, 0, pk1);
  add_pk_to_auth_data(auth_data2, 0, pk2);

  auth_data1.required_weight = 1;

  auth_data2.required_weight = 1;

  // address3 has no auth data
  ContractAddress address3 = make_test_address(10);

  mock_db().set(address1, auth_data1);
  mock_db().set(address2, auth_data2);
  mock_db().set(address3, AddressAuthData{});
  mock_db().compute_key_changes_for_current_height();

  auth_server().register_auth_data_updates(0,
                                           mock_db().get_key_changes_at_height(
                                               0))();

  Transaction tx1{};
  tx1.invocation.invoked_address = address1;
  tx1.auth_claim = make_self_individual_key_claim(0);

  Transaction tx2{};
  tx2.invocation.invoked_address = address2;
  tx2.auth_claim = make_self_individual_key_claim(0);

  Transaction tx3{};
  tx3.invocation.invoked_address = address3;
  tx3.auth_claim = make_self_individual_key_claim(0);

  // tx1 is signed by the wrong key
  SignedTransaction stx1{};
  stx1.tx = tx1;
  stx1.signatures.push_back(make_signature(sk2, tx1, /*public_key_id=*/0));

  SignedTransaction stx2{};
  stx2.tx = tx2;
  stx2.signatures.push_back(make_signature(sk2, tx2, /*public_key_id=*/0));

  SignedTransaction stx3{};
  stx3.tx = tx3;
  stx3.signatures.push_back(make_signature(sk2, tx3, /*public_key_id=*/0));

  x23::Vec<SignedTransaction> stx_vec{stx1, stx2, stx3};
  x23::span<SignedTransaction> stx_span(stx_vec);
  auto result = auth_server().verify_transactions(0, stx_span)();

  ASSERT_EQ(result.size(), 3);
  EXPECT_EQ(result[0], AuthErrorCode::kAuthFailed);
  EXPECT_EQ(result[1], AuthErrorCode::kAuthSuccess);
  EXPECT_EQ(result[2], AuthErrorCode::kAuthFailed);

  // batch verify
  EXPECT_EQ(auth_server().verify_batch(0, {stx1, stx2, stx3})(),
            AuthErrorCode::kAuthFailed);
  EXPECT_EQ(auth_server().verify_batch(0, {stx2, stx3})(),
            AuthErrorCode::kAuthFailed);
  EXPECT_EQ(auth_server().verify_batch(0, {stx2, stx2, stx2})(),
            AuthErrorCode::kAuthSuccess);
}

TEST_F(AuthVerifyTest, ParallelVerifyTransactionsMapDataRace)
{
  constexpr size_t kNumAddresses = 4;
  constexpr size_t kTxsPerAddress = 16;
  constexpr size_t kTotalTxs = kNumAddresses * kTxsPerAddress;
  constexpr size_t kMaxParallelism = 4;

  ContractAddress addresses[kNumAddresses];
  std::pair<PublicKey, SecretKey> keys[kNumAddresses];

  for (size_t i = 0; i < kNumAddresses; i++) {
    addresses[i] = make_test_address(static_cast<uint8_t>(20 + i));

    auto [pk, sk] = make_key_pair();
    keys[i] = {pk, sk};

    AddressAuthData auth_data{};
    add_pk_to_auth_data(auth_data, 0, pk);
    auth_data.required_weight = 1;

    mock_db().set(addresses[i], auth_data);
  }

  mock_db().compute_key_changes_for_current_height();
  auth_server().register_auth_data_updates(0,
                                           mock_db().get_key_changes_at_height(
                                               0))();

  x23::Vec<SignedTransaction> stx_vec;
  for (size_t i = 0; i < kTotalTxs; i++) {
    const size_t addr_idx = i % kNumAddresses;

    Transaction tx{};
    tx.invocation.invoked_address = addresses[addr_idx];
    tx.auth_claim = make_self_individual_key_claim(0);

    SignedTransaction stx{};
    stx.tx = tx;
    stx.signatures.push_back(make_signature(keys[addr_idx].second, tx, 0, 0));
    stx_vec.push_back(std::move(stx));
  }

  x23::span<SignedTransaction> stx_span(stx_vec);
  const auto result =
      auth_server().verify_transactions(0, stx_span, kMaxParallelism)();

  ASSERT_EQ(result.size(), kTotalTxs);
  for (AuthErrorCode status : result) {
    EXPECT_EQ(status, AuthErrorCode::kAuthSuccess);
  }
}

} // namespace hogd
