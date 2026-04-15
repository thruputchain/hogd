#include <gtest/gtest.h>

#include <cassert>
#include <limits>
#include <utility>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/auth_claim/status.h"
#include "hogd/auth_claim/verify.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/init.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

class AuthClaimTests : public ::testing::Test {
protected:
  void SetUp() override {}

  static AuthClaim make_individual_key_claim(uint32_t key_id)
  {
    AuthClaim claim(AuthClaimType::kInvokedAddressSignature);
    claim.invoked_address_signature_id() = key_id;
    return claim;
  }

  static AuthClaim make_aux_individual_key_claim(uint32_t key_id,
                                                 ContractAddress address)
  {
    AuthClaim claim(AuthClaimType::kOtherAddressSignature);
    claim.other_address_signature_id().key_id = key_id;
    claim.other_address_signature_id().address = address;
    return claim;
  }

  static AuthClaim make_weight_key_claim(uint32_t key_id, Weight weight)
  {
    AuthClaim claim(AuthClaimType::kInvokedAddressSignedWeight);
    claim.invoked_address_signed_weight().accepted_keys =
        static_cast<uint32_t>(1) << key_id;
    claim.invoked_address_signed_weight().required_weight = weight;
    return claim;
  }

  static Transaction make_transaction(const x23::Vec<AuthClaim> &auth_claims)
  {
    Transaction tx{};
    tx.auth_claims = auth_claims;
    return tx;
  }

  static DecoratedSignature make_signature(SecretKey sk, Transaction const &tx,
                                           uint32_t auth_claim_idx,
                                           uint32_t public_key_id)
  {
    Signature sig(CryptographicScheme::Ed25519);
    assert(sk.scheme() == CryptographicScheme::Ed25519);
    sig.ed25519() = ed25519_sign(sk.ed25519(), x23::serialize(tx));
    return {.auth_claim_idx = auth_claim_idx,
            .public_key_id = public_key_id,
            .signature = sig};
  }

  static std::pair<PublicKey, SecretKey> make_key_pair()
  {
    auto [pk, sk] = ed25519_keygen();
    PublicKey pk_wrapper(CryptographicScheme::Ed25519);
    pk_wrapper.ed25519() = pk;
    SecretKey sk_wrapper(CryptographicScheme::Ed25519);
    sk_wrapper.ed25519() = sk;
    return {pk_wrapper, sk_wrapper};
  }

  constexpr static NonceConfig kAnyKeyNonceConfig =
      NonceConfig{.accepted_keys = 0x000F'FFFF, .required_weight = 0};
};

TEST_F(AuthClaimTests, IndividualKeyClaim)
{
  AuthClaim claim = make_individual_key_claim(0);

  auto [pk, sk] = make_key_pair();
  auto tx = make_transaction({claim});
  DecoratedSignature sig =
      make_signature(sk, tx, /*auth_claim_idx=*/0, /*public_key_id=*/0);

  SignedTransaction stx{.tx = make_transaction({claim}), .signatures = {sig}};

  EXPECT_TRUE(auth_claim_is_well_formed(stx).has_value());

  auto [pk2, _] = make_key_pair();

  AddressAuthData good_data =
      AddressAuthData{.public_keys = {x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk,
                                              .max_weight = Weight{1}})},
                      .nonce_config = kAnyKeyNonceConfig};

  AddressAuthData missing_key =
      AddressAuthData{.nonce_config = kAnyKeyNonceConfig};

  EXPECT_TRUE(lightweight_validate_auth_claims(stx, good_data, {}).has_value());
  EXPECT_FALSE(
      lightweight_validate_auth_claims(stx, missing_key, {}).has_value());
  EXPECT_EQ(
      lightweight_validate_auth_claims(stx, missing_key, {}).error().code(),
      AuthVerifyErrorCode::kAuthDataMismatch);

  AddressAuthData bad_data =
      AddressAuthData{.public_keys = {x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk2,
                                              .max_weight = Weight{1}})},
                      .nonce_config = kAnyKeyNonceConfig};

  EXPECT_TRUE(verify_auth(stx, good_data, {}).has_value());
  EXPECT_FALSE(verify_auth(stx, bad_data, {}).has_value());
  EXPECT_EQ(verify_auth(stx, bad_data, {}).error().code(),
            AuthVerifyErrorCode::kSignatureVerificationFailed);
}

TEST_F(AuthClaimTests, GasAllSponsored)
{
  AuthClaim claim = make_aux_individual_key_claim(0, ContractAddress{1});
  auto aux_contract = ContractAddress{1};

  auto [pk, sk] = make_key_pair();
  auto [pk2, sk2] = make_key_pair();
  auto tx = make_transaction({claim});
  tx.gas_claims.push_back(
      GasClaim{.sponsored_contract = aux_contract, .amount = 100});
  tx.invocation.gas_limit = 100;

  DecoratedSignature sig =
      make_signature(sk, tx, /*auth_claim_idx=*/0, /*public_key_id=*/0);

  SignedTransaction stx{.tx = tx, .signatures = {sig}};

  EXPECT_TRUE(auth_claim_is_well_formed(stx).has_value());

  AddressAuthData aux_data = AddressAuthData{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
          new PublicKeyConfig{.key = pk, .max_weight = Weight{1}})},
      .gas_permissions = {GasPermissionConfig{.limit = 100,
                                              .accepted_keys = 1 << 0,
                                              .required_weight = 1}},
      .nonce_config = kAnyKeyNonceConfig,
  };

  AddressAuthData self_data = AddressAuthData{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
          new PublicKeyConfig{.key = pk2, .max_weight = Weight{1}})},
      .gas_permissions = {GasPermissionConfig{.limit = 2,
                                              .accepted_keys = 1 << 0,
                                              .required_weight = 10}},
      .nonce_config = kAnyKeyNonceConfig,
  };

  EXPECT_TRUE(auth_claim_is_well_formed(stx).has_value());
  EXPECT_TRUE(lightweight_validate_auth_claims(stx, self_data,
                                               {{aux_contract, &aux_data}})
                  .has_value());
  EXPECT_TRUE(
      verify_auth(stx, self_data, {{aux_contract, &aux_data}}).has_value());
}

TEST_F(AuthClaimTests, GasSponsorInsufficient)
{
  AuthClaim claim = make_aux_individual_key_claim(0, ContractAddress{1});
  auto aux_contract = ContractAddress{1};

  auto [pk, sk] = make_key_pair();
  auto [pk2, sk2] = make_key_pair();
  auto tx = make_transaction({claim});
  tx.gas_claims.push_back(
      GasClaim{.sponsored_contract = aux_contract, .amount = 100});
  tx.invocation.gas_limit = 200;

  DecoratedSignature sig =
      make_signature(sk, tx, /*auth_claim_idx=*/0, /*public_key_id=*/0);

  SignedTransaction stx{.tx = tx, .signatures = {sig}};

  EXPECT_TRUE(auth_claim_is_well_formed(stx).has_value());

  AddressAuthData aux_data = AddressAuthData{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
          new PublicKeyConfig{.key = pk, .max_weight = Weight{1}})},
      .gas_permissions = {GasPermissionConfig{.limit = 100,
                                              .accepted_keys = 1 << 0,
                                              .required_weight = 1}},
      .nonce_config = kAnyKeyNonceConfig,
  };

  AddressAuthData self_data = AddressAuthData{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
          new PublicKeyConfig{.key = pk2, .max_weight = Weight{1}})},
      .gas_permissions = {GasPermissionConfig{.limit = 2,
                                              .accepted_keys = 1 << 0,
                                              .required_weight = 10}},
      .nonce_config = kAnyKeyNonceConfig,
  };

  // Gas claims can have 0 weight required, and the sponsored claim is implicit,
  // so this auth data is plausible.
  EXPECT_TRUE(auth_claim_is_well_formed(stx).has_value());
  ASSERT_FALSE(lightweight_validate_auth_claims(stx, self_data,
                                                {{aux_contract, &aux_data}})
                   .has_value());
  ASSERT_EQ(lightweight_validate_auth_claims(stx, self_data,
                                             {{aux_contract, &aux_data}})
                .error()
                .code(),
            AuthVerifyErrorCode::kAuthDataMismatch);
}

TEST_F(AuthClaimTests, BatchSignatureVerification)
{
  AuthClaim claim = make_individual_key_claim(0);

  auto [pk, sk] = make_key_pair();
  auto tx = make_transaction({claim});
  DecoratedSignature sig =
      make_signature(sk, tx, /*auth_claim_idx=*/0, /*public_key_id=*/0);

  SignedTransaction stx{.tx = make_transaction({claim}), .signatures = {sig}};

  EXPECT_TRUE(auth_claim_is_well_formed(stx).has_value());

  AddressAuthData good_data =
      AddressAuthData{.public_keys = {x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk,
                                              .max_weight = Weight{1}})},
                      .nonce_config = kAnyKeyNonceConfig};

  {
    Ed25519SignatureVerifier signature_verifier;
    EXPECT_TRUE(
        verify_auth(stx, good_data, {}, &signature_verifier).has_value());

    EXPECT_TRUE(signature_verifier.verify());
  }
  auto [pk2, sk2] = make_key_pair();
  DecoratedSignature sig2 =
      make_signature(sk2, tx, /*auth_claim_idx=*/0, /*public_key_id=*/0);

  SignedTransaction stx2{.tx = make_transaction({claim}), .signatures = {sig2}};

  EXPECT_TRUE(auth_claim_is_well_formed(stx2).has_value());

  {
    Ed25519SignatureVerifier signature_verifier;
    // If we pass in the batch signature verifier, then signatures aren't
    // checked during verify_auth; they're instead checked when verify() is
    // called on the verifier.
    EXPECT_TRUE(
        verify_auth(stx2, good_data, {}, &signature_verifier).has_value());
    EXPECT_FALSE(signature_verifier.verify());
  }
}

// Regression test for issue #1443: public_key_id >= 32 caused UB in
// bitmap_accepts_key via 1 << idx with out-of-range idx.
TEST_F(AuthClaimTests, PublicKeyIdShiftUB)
{
  AuthClaim claim(AuthClaimType::kInvokedAddressSignedWeight);
  claim.invoked_address_signed_weight().accepted_keys = 0x1;
  claim.invoked_address_signed_weight().required_weight = 1;

  auto [pk, sk] = make_key_pair();
  auto tx = make_transaction({claim});
  DecoratedSignature sig =
      make_signature(sk, tx, /*auth_claim_idx=*/0, /*public_key_id=*/32);

  SignedTransaction stx{.tx = tx, .signatures = {sig}};

  auto result = auth_claim_is_well_formed(stx);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), AuthVerifyErrorCode::kAuthMalformed);
}

// Verify that a weighted claim rejects public_key_id that is within bitmap
// range but not set in accepted_keys.
TEST_F(AuthClaimTests, WeightedClaimRejectsUnacceptedKey)
{
  // accepted_keys bitmap only has bit 0 set
  AuthClaim claim = make_weight_key_claim(0, Weight{1});

  auto [pk, sk] = make_key_pair();
  auto tx = make_transaction({claim});
  // Sign with public_key_id=5, which is valid range but not in accepted_keys
  DecoratedSignature sig =
      make_signature(sk, tx, /*auth_claim_idx=*/0, /*public_key_id=*/5);

  SignedTransaction stx{.tx = tx, .signatures = {sig}};

  auto result = auth_claim_is_well_formed(stx);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), AuthVerifyErrorCode::kAuthMalformed);
}

TEST_F(AuthClaimTests, WeightedClaimRejectsDuplicateSignerInBucket)
{
  AuthClaim real_claim(AuthClaimType::kInvokedAddressSignedWeight);
  real_claim.invoked_address_signed_weight().accepted_keys = 1 << 0;
  real_claim.invoked_address_signed_weight().required_weight = 1;

  AuthClaim dummy_claim(AuthClaimType::kInvokedAddressSignedWeight);
  dummy_claim.invoked_address_signed_weight().accepted_keys = 0;
  dummy_claim.invoked_address_signed_weight().required_weight = 0;

  auto [ignored_pk, sk] = make_key_pair();
  auto tx = make_transaction({real_claim, dummy_claim});

  DecoratedSignature sig0 =
      make_signature(sk, tx, /*auth_claim_idx=*/0, /*public_key_id=*/0);
  DecoratedSignature sig1 = sig0;

  SignedTransaction stx{.tx = tx, .signatures = {sig0, sig1}};

  auto result = auth_claim_is_well_formed(stx);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), AuthVerifyErrorCode::kAuthMalformed);
}

TEST_F(AuthClaimTests, WeightedClaimRejectsOutOfOrderSignerInBucket)
{
  AuthClaim real_claim(AuthClaimType::kInvokedAddressSignedWeight);
  real_claim.invoked_address_signed_weight().accepted_keys =
      (1 << 0) | (1 << 1);
  real_claim.invoked_address_signed_weight().required_weight = 2;

  AuthClaim dummy_claim(AuthClaimType::kInvokedAddressSignedWeight);
  dummy_claim.invoked_address_signed_weight().accepted_keys = 0;
  dummy_claim.invoked_address_signed_weight().required_weight = 0;

  auto [ignored_pk0, sk0] = make_key_pair();
  auto [ignored_pk1, sk1] = make_key_pair();
  auto tx = make_transaction({real_claim, dummy_claim});

  DecoratedSignature sig1 =
      make_signature(sk1, tx, /*auth_claim_idx=*/0, /*public_key_id=*/1);
  DecoratedSignature sig0 =
      make_signature(sk0, tx, /*auth_claim_idx=*/0, /*public_key_id=*/0);

  SignedTransaction stx{.tx = tx, .signatures = {sig1, sig0}};

  auto result = auth_claim_is_well_formed(stx);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), AuthVerifyErrorCode::kAuthMalformed);
}

// Verify that very large public_key_id values (UINT32_MAX) are rejected
// cleanly without UB.
TEST_F(AuthClaimTests, PublicKeyIdMaxValueRejected)
{
  AuthClaim claim(AuthClaimType::kInvokedAddressSignedWeight);
  claim.invoked_address_signed_weight().accepted_keys = 0x1;
  claim.invoked_address_signed_weight().required_weight = 1;

  auto [pk, sk] = make_key_pair();
  auto tx = make_transaction({claim});
  DecoratedSignature sig =
      make_signature(sk, tx, /*auth_claim_idx=*/0,
                     /*public_key_id=*/std::numeric_limits<uint32_t>::max());

  SignedTransaction stx{.tx = tx, .signatures = {sig}};

  auto result = auth_claim_is_well_formed(stx);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), AuthVerifyErrorCode::kAuthMalformed);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
