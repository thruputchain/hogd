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

  static AuthClaim make_claim(uint32_t allowed_keys, Weight required_weight = 1)
  {
    AuthClaim claim(AuthClaimType::kSourceAddressAuthData);
    claim.source_address_claim().allowed_keys = allowed_keys;
    claim.source_address_claim().required_weight = required_weight;
    return claim;
  }

  static Transaction make_transaction(AuthClaim auth_claim)
  {
    Transaction tx{};
    tx.auth_claim = auth_claim;
    return tx;
  }

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  static DecoratedSignature make_signature(SecretKey sk, Transaction const &tx,
                                           uint32_t public_key_id)
  {
    Signature sig(CryptographicScheme::Ed25519);
    assert(sk.scheme() == CryptographicScheme::Ed25519);
    sig.ed25519() = ed25519_sign(sk.ed25519(), x23::serialize(tx)).value();
    return {.public_key_id = public_key_id, .signature = sig};
  }
  // NOLINTEND(bugprone-unchecked-optional-access)

  static std::pair<PublicKey, SecretKey> make_key_pair()
  {
    auto [pk, sk] = ed25519_keygen();
    PublicKey pk_wrapper(CryptographicScheme::Ed25519);
    pk_wrapper.ed25519() = pk;
    SecretKey sk_wrapper(CryptographicScheme::Ed25519);
    sk_wrapper.ed25519() = sk;
    return {pk_wrapper, sk_wrapper};
  }
};

TEST_F(AuthClaimTests, IndividualKeyClaim)
{
  AuthClaim claim = make_claim(1 << 0);

  auto [pk, sk] = make_key_pair();
  auto tx = make_transaction(claim);
  DecoratedSignature sig = make_signature(sk, tx, /*public_key_id=*/0);

  SignedTransaction stx{.tx = tx, .signatures = {sig}};

  EXPECT_TRUE(auth_claim_is_well_formed(stx).has_value());

  auto [pk2, _] = make_key_pair();

  AddressAuthData good_data =
      AddressAuthData{.public_keys = {x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk,
                                              .max_weight = Weight{1}})},
                      .required_weight = 1};

  AddressAuthData missing_key = AddressAuthData{.required_weight = 1};

  EXPECT_TRUE(lightweight_validate_auth_claims(stx, good_data).has_value());
  EXPECT_FALSE(lightweight_validate_auth_claims(stx, missing_key).has_value());
  EXPECT_EQ(lightweight_validate_auth_claims(stx, missing_key).error().code(),
            AuthVerifyErrorCode::kAuthDataMismatch);

  AddressAuthData bad_data =
      AddressAuthData{.public_keys = {x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk2,
                                              .max_weight = Weight{1}})},
                      .required_weight = 1};

  EXPECT_TRUE(verify_auth(stx, good_data).has_value());
  EXPECT_FALSE(verify_auth(stx, bad_data).has_value());
  EXPECT_EQ(verify_auth(stx, bad_data).error().code(),
            AuthVerifyErrorCode::kSignatureVerificationFailed);
}

TEST_F(AuthClaimTests, BatchSignatureVerification)
{
  AuthClaim claim = make_claim(1 << 0);

  auto [pk, sk] = make_key_pair();
  auto tx = make_transaction(claim);
  DecoratedSignature sig = make_signature(sk, tx, /*public_key_id=*/0);

  SignedTransaction stx{.tx = tx, .signatures = {sig}};

  EXPECT_TRUE(auth_claim_is_well_formed(stx).has_value());

  AddressAuthData good_data =
      AddressAuthData{.public_keys = {x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk,
                                              .max_weight = Weight{1}})},
                      .required_weight = 1};

  {
    Ed25519SignatureVerifier signature_verifier;
    EXPECT_TRUE(verify_auth(stx, good_data, &signature_verifier).has_value());
    EXPECT_TRUE(signature_verifier.verify());
  }

  auto [pk2, sk2] = make_key_pair();
  DecoratedSignature sig2 = make_signature(sk2, tx, /*public_key_id=*/0);

  SignedTransaction stx2{.tx = tx, .signatures = {sig2}};

  EXPECT_TRUE(auth_claim_is_well_formed(stx2).has_value());

  {
    Ed25519SignatureVerifier signature_verifier;
    EXPECT_TRUE(verify_auth(stx2, good_data, &signature_verifier).has_value());
    EXPECT_FALSE(signature_verifier.verify());
  }
}

TEST_F(AuthClaimTests, PublicKeyIdShiftUB)
{
  AuthClaim claim = make_claim(0x1);

  auto [pk, sk] = make_key_pair();
  auto tx = make_transaction(claim);
  DecoratedSignature sig = make_signature(sk, tx, /*public_key_id=*/32);

  SignedTransaction stx{.tx = tx, .signatures = {sig}};

  auto result = auth_claim_is_well_formed(stx);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), AuthVerifyErrorCode::kAuthMalformed);
}

TEST_F(AuthClaimTests, WeightedClaimRejectsUnacceptedKey)
{
  AuthClaim claim = make_claim(1 << 0);

  auto [pk, sk] = make_key_pair();
  auto tx = make_transaction(claim);
  DecoratedSignature sig = make_signature(sk, tx, /*public_key_id=*/5);

  SignedTransaction stx{.tx = tx, .signatures = {sig}};

  auto result = auth_claim_is_well_formed(stx);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), AuthVerifyErrorCode::kAuthMalformed);
}

TEST_F(AuthClaimTests, DuplicateSignerRejected)
{
  AuthClaim claim = make_claim(1 << 0);

  auto [pk, sk] = make_key_pair();
  auto tx = make_transaction(claim);

  DecoratedSignature sig0 = make_signature(sk, tx, /*public_key_id=*/0);
  DecoratedSignature sig1 = sig0;

  SignedTransaction stx{.tx = tx, .signatures = {sig0, sig1}};

  auto result = auth_claim_is_well_formed(stx);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), AuthVerifyErrorCode::kAuthMalformed);
}

TEST_F(AuthClaimTests, OutOfOrderSignerRejected)
{
  AuthClaim claim = make_claim((1 << 0) | (1 << 1));

  auto [pk0, sk0] = make_key_pair();
  auto [pk1, sk1] = make_key_pair();
  auto tx = make_transaction(claim);

  DecoratedSignature sig1 = make_signature(sk1, tx, /*public_key_id=*/1);
  DecoratedSignature sig0 = make_signature(sk0, tx, /*public_key_id=*/0);

  SignedTransaction stx{.tx = tx, .signatures = {sig1, sig0}};

  auto result = auth_claim_is_well_formed(stx);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), AuthVerifyErrorCode::kAuthMalformed);
}

TEST_F(AuthClaimTests, ClaimRequiredWeightEnforced)
{
  AuthClaim claim = make_claim((1 << 0) | (1 << 1), /*required_weight=*/3);

  auto [pk0, sk0] = make_key_pair();
  auto [pk1, sk1] = make_key_pair();
  auto tx = make_transaction(claim);
  DecoratedSignature sig0 = make_signature(sk0, tx, /*public_key_id=*/0);

  AddressAuthData auth_data{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk0, .max_weight = 2}),
                      x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk1, .max_weight = 2})},
      .required_weight = 1,
  };

  // One signature (weight 2) doesn't meet claim's required_weight of 3
  SignedTransaction stx1{.tx = tx, .signatures = {sig0}};
  EXPECT_FALSE(lightweight_validate_auth_claims(stx1, auth_data).has_value());

  // Two signatures (weight 4) meets claim's required_weight of 3
  DecoratedSignature sig1 = make_signature(sk1, tx, /*public_key_id=*/1);
  SignedTransaction stx2{.tx = tx, .signatures = {sig0, sig1}};
  EXPECT_TRUE(lightweight_validate_auth_claims(stx2, auth_data).has_value());
}

TEST_F(AuthClaimTests, PublicKeyIdMaxValueRejected)
{
  AuthClaim claim = make_claim(0x1);

  auto [pk, sk] = make_key_pair();
  auto tx = make_transaction(claim);
  DecoratedSignature sig =
      make_signature(sk, tx,
                     /*public_key_id=*/std::numeric_limits<uint32_t>::max());

  SignedTransaction stx{.tx = tx, .signatures = {sig}};

  auto result = auth_claim_is_well_formed(stx);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), AuthVerifyErrorCode::kAuthMalformed);
}

TEST_F(AuthClaimTests, AccountRequiredWeightEnforced)
{
  // Claim requires weight 1, but account requires weight 5.
  // The max of the two (5) should be enforced.
  AuthClaim claim = make_claim((1 << 0) | (1 << 1), /*required_weight=*/1);

  auto [pk0, sk0] = make_key_pair();
  auto [pk1, sk1] = make_key_pair();
  auto tx = make_transaction(claim);

  AddressAuthData auth_data{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk0, .max_weight = 2}),
                      x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk1, .max_weight = 2})},
      .required_weight = 5,
  };

  // One sig (weight 2) passes claim's threshold (1) but not account's (5)
  DecoratedSignature sig0 = make_signature(sk0, tx, /*public_key_id=*/0);
  SignedTransaction stx1{.tx = tx, .signatures = {sig0}};
  EXPECT_FALSE(lightweight_validate_auth_claims(stx1, auth_data).has_value());

  // Two sigs (weight 4) still below account's required_weight of 5
  DecoratedSignature sig1 = make_signature(sk1, tx, /*public_key_id=*/1);
  SignedTransaction stx2{.tx = tx, .signatures = {sig0, sig1}};
  EXPECT_FALSE(lightweight_validate_auth_claims(stx2, auth_data).has_value());

  // Bump key weights so total (6) meets account threshold
  AddressAuthData auth_data2{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk0, .max_weight = 3}),
                      x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk1, .max_weight = 3})},
      .required_weight = 5,
  };
  EXPECT_TRUE(lightweight_validate_auth_claims(stx2, auth_data2).has_value());
}

TEST_F(AuthClaimTests, MultiSigHappyPath)
{
  // 2-of-3 multisig: 3 keys with weight 1 each, required_weight = 2
  auto [pk0, sk0] = make_key_pair();
  auto [pk1, sk1] = make_key_pair();
  auto [pk2, sk2] = make_key_pair();

  AuthClaim claim = make_claim((1 << 0) | (1 << 1) | (1 << 2),
                               /*required_weight=*/2);
  auto tx = make_transaction(claim);

  AddressAuthData auth_data{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk0, .max_weight = 1}),
                      x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk1, .max_weight = 1}),
                      x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk2, .max_weight = 1})},
      .required_weight = 1,
  };

  // Keys 0 and 2 sign (skipping key 1)
  DecoratedSignature sig0 = make_signature(sk0, tx, /*public_key_id=*/0);
  DecoratedSignature sig2 = make_signature(sk2, tx, /*public_key_id=*/2);
  SignedTransaction stx{.tx = tx, .signatures = {sig0, sig2}};

  EXPECT_TRUE(auth_claim_is_well_formed(stx).has_value());
  EXPECT_TRUE(lightweight_validate_auth_claims(stx, auth_data).has_value());
  EXPECT_TRUE(verify_auth(stx, auth_data).has_value());
}

TEST_F(AuthClaimTests, InsufficientTotalWeight)
{
  // Two keys with weight 1 each, but required_weight is 3
  auto [pk0, sk0] = make_key_pair();
  auto [pk1, sk1] = make_key_pair();

  AuthClaim claim = make_claim((1 << 0) | (1 << 1), /*required_weight=*/3);
  auto tx = make_transaction(claim);

  AddressAuthData auth_data{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk0, .max_weight = 1}),
                      x23::Ptr<PublicKeyConfig>(
                          new PublicKeyConfig{.key = pk1, .max_weight = 1})},
      .required_weight = 1,
  };

  DecoratedSignature sig0 = make_signature(sk0, tx, /*public_key_id=*/0);
  DecoratedSignature sig1 = make_signature(sk1, tx, /*public_key_id=*/1);
  SignedTransaction stx{.tx = tx, .signatures = {sig0, sig1}};

  EXPECT_TRUE(auth_claim_is_well_formed(stx).has_value());
  auto result = lightweight_validate_auth_claims(stx, auth_data);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), AuthVerifyErrorCode::kAuthDataMismatch);
}

TEST_F(AuthClaimTests, InvalidBitmapRejected)
{
  // Bitmap with bit 21 set (beyond kMaxSigners)
  AuthClaim claim = make_claim(1U << kMaxSigners);

  auto [pk, sk] = make_key_pair();
  auto tx = make_transaction(claim);
  DecoratedSignature sig = make_signature(sk, tx, /*public_key_id=*/0);
  SignedTransaction stx{.tx = tx, .signatures = {sig}};

  auto result = auth_claim_is_well_formed(stx);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), AuthVerifyErrorCode::kAuthMalformed);
}

TEST_F(AuthClaimTests, ZeroSignaturesRejectedByWeight)
{
  AuthClaim claim = make_claim(1 << 0);
  auto tx = make_transaction(claim);

  auto [pk, sk] = make_key_pair();
  AddressAuthData auth_data{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
          new PublicKeyConfig{.key = pk, .max_weight = 1})},
      .required_weight = 1,
  };

  SignedTransaction stx{.tx = tx, .signatures = {}};

  // Well-formed (no structural issue with zero sigs)
  EXPECT_TRUE(auth_claim_is_well_formed(stx).has_value());

  // But fails weight check
  auto result = lightweight_validate_auth_claims(stx, auth_data);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), AuthVerifyErrorCode::kAuthDataMismatch);
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
