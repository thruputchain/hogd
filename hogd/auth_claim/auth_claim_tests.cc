#include <cassert>
#include <gtest/gtest.h>
#include <utility>

#include "account.hh"
#include "auth_claim.hh"
#include "hogd/auth_claim/verify.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/init.h"
#include "transaction.hh"
#include "types.hh"
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

  static AuthClaim make_weight_key_claim(uint32_t key_id, Weight weight)
  {
    AuthClaim claim(AuthClaimType::kInvokedAddressSignedWeight);
    claim.invoked_address_signed_weight().accepted_keys = 1 << key_id;
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
                                           uint32_t pk_id)
  {
    Signature sig(CryptographicScheme::Ed25519);
    assert(sk.scheme() == CryptographicScheme::Ed25519);
    auto msg = sha256(tx);
    sig.ed25519() = ed25519_sign(sk.ed25519(), msg);
    return {.public_key_id = pk_id, .signature = sig};
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
};

TEST_F(AuthClaimTests, IndividualKeyClaim)
{
  AuthClaim claim = make_individual_key_claim(0);

  auto [pk, sk] = make_key_pair();
  auto tx = make_transaction({claim});
  auto sig = make_signature(sk, tx, 0);

  SignedTransaction stx =
      SignedTransaction{.tx = make_transaction({claim}), .signatures = {{sig}}};

  EXPECT_TRUE(lightweight_plausible_auth_claims(stx));

  auto [pk2, _] = make_key_pair();

  AccountAuthData good_data = AccountAuthData{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
          new PublicKeyConfig{.key = pk, .max_weight = Weight{1}})}};

  AccountAuthData missing_key = AccountAuthData{};

  EXPECT_TRUE(lightweight_validate_auth_claims(stx, good_data, {}));
  EXPECT_FALSE(lightweight_validate_auth_claims(stx, missing_key, {}));

  AccountAuthData bad_data = AccountAuthData{
      .public_keys = {x23::Ptr<PublicKeyConfig>(
          new PublicKeyConfig{.key = pk2, .max_weight = Weight{1}})}};

  EXPECT_TRUE(verify_auth(stx, good_data, {}));
  EXPECT_FALSE(verify_auth(stx, bad_data, {}));
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
