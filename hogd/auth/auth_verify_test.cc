#include <gtest/gtest.h>

#include <utility>

#include "account.hh"
#include "auth_claim.hh"
#include "auth_rpc.hh"
#include "auth_server_test_framework.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/x23.h"

namespace hogd {

namespace {
constexpr ContractAddress
make_test_address(uint8_t id)
{
  ContractAddress address{};
  address[0] = id;
  return address;
}

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
  AuthClaim claim(AuthClaimType::kInvokedAddressSignature);
  claim.invoked_address_signature_id() = id;
  return claim;
}

constexpr DecoratedSignature
make_signature(SecretKey sk, Transaction const &tx, uint8_t id)
{
  DecoratedSignature signature;
  signature.public_key_id = id;
  signature.signature = Signature(CryptographicScheme::Ed25519);
  signature.signature.ed25519() = ed25519_sign(sk.ed25519(), sha256(tx));
  return signature;
}

void
add_pk_to_auth_data(AccountAuthData &auth_data, uint8_t id, PublicKey const &pk)
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

  AccountAuthData auth_data{};
  auto [pk, sk] = make_key_pair();
  add_pk_to_auth_data(auth_data, 0, pk);

  auth_data.nonce_config.accepted_keys = 1 << 0;
  auth_data.nonce_config.required_weight = 1;

  auth_data.gas_permissions.push_back(GasPermissionConfig{
      .limit = 100, .accepted_keys = 1 << 0, .required_weight = 1});

  mock_db().set(address, auth_data);
  mock_db().increment_height();

  auth_server().register_auth_data_updates(1,
                                           mock_db().get_key_changes_at_height(
                                               1))();

  Transaction tx{};
  tx.invocation.invoked_address = address;
  tx.auth_claims.push_back(make_self_individual_key_claim(0));

  SignedTransaction stx{};
  stx.tx = tx;
  DecoratedSignatureList decorated_signatures;
  decorated_signatures.list.push_back(make_signature(sk, tx, 0));
  stx.signatures.push_back(decorated_signatures);

  auto result = auth_server().verify_transactions(1, {stx})();
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0], AuthErrorCode::kAuthSuccess);
}

TEST_F(AuthVerifyTest, VerifyMultipleTransaction)
{
  ContractAddress address1 = make_test_address(8);
  ContractAddress address2 = make_test_address(9);

  AccountAuthData auth_data1{};
  AccountAuthData auth_data2{};

  auto [pk1, sk1] = make_key_pair();
  auto [pk2, sk2] = make_key_pair();

  add_pk_to_auth_data(auth_data1, 0, pk1);
  add_pk_to_auth_data(auth_data2, 0, pk2);

  auth_data1.nonce_config.accepted_keys = 1 << 0;
  auth_data1.nonce_config.required_weight = 1;

  auth_data1.gas_permissions.push_back(GasPermissionConfig{
      .limit = 100, .accepted_keys = 1 << 0, .required_weight = 1});

  auth_data2.nonce_config.accepted_keys = 1 << 0;
  auth_data2.nonce_config.required_weight = 1;

  auth_data2.gas_permissions.push_back(GasPermissionConfig{
      .limit = 100, .accepted_keys = 1 << 0, .required_weight = 1});

  // address3 has no auth data
  ContractAddress address3 = make_test_address(10);

  mock_db().set(address1, auth_data1);
  mock_db().set(address2, auth_data2);
  mock_db().set(address3, AccountAuthData{});
  mock_db().increment_height();

  auth_server().register_auth_data_updates(1,
                                           mock_db().get_key_changes_at_height(
                                               1))();

  Transaction tx1{};
  tx1.invocation.invoked_address = address1;
  tx1.auth_claims.push_back(make_self_individual_key_claim(0));

  Transaction tx2{};
  tx2.invocation.invoked_address = address2;
  tx2.auth_claims.push_back(make_self_individual_key_claim(0));

  Transaction tx3{};
  tx3.invocation.invoked_address = address3;
  tx3.auth_claims.push_back(make_self_individual_key_claim(0));

  // tx1 is signed by the wrong key
  SignedTransaction stx1{};
  stx1.tx = tx1;
  DecoratedSignatureList decorated_signatures1;
  decorated_signatures1.list.push_back(make_signature(sk2, tx1, 0));
  stx1.signatures.push_back(decorated_signatures1);

  SignedTransaction stx2{};
  stx2.tx = tx2;
  DecoratedSignatureList decorated_signatures2;
  decorated_signatures2.list.push_back(make_signature(sk2, tx2, 0));
  stx2.signatures.push_back(decorated_signatures2);

  SignedTransaction stx3{};
  stx3.tx = tx3;
  DecoratedSignatureList decorated_signatures3;
  decorated_signatures3.list.push_back(make_signature(sk2, tx3, 0));
  stx3.signatures.push_back(decorated_signatures3);

  auto result = auth_server().verify_transactions(1, {stx1, stx2, stx3})();

  ASSERT_EQ(result.size(), 3);
  EXPECT_EQ(result[0], AuthErrorCode::kAuthFailed);
  EXPECT_EQ(result[1], AuthErrorCode::kAuthSuccess);
  EXPECT_EQ(result[2], AuthErrorCode::kAuthFailed);

  // batch verify
  EXPECT_EQ(auth_server().verify_batch(1, {stx1, stx2, stx3})(),
            AuthErrorCode::kAuthFailed);
  EXPECT_EQ(auth_server().verify_batch(1, {stx2, stx3})(),
            AuthErrorCode::kAuthFailed);
  EXPECT_EQ(auth_server().verify_batch(1, {stx2, stx2, stx2})(),
            AuthErrorCode::kAuthSuccess);
}

} // namespace hogd
