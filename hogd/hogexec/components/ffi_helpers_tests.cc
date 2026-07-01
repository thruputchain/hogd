#include <gtest/gtest.h>

#include <cstring>
#include <limits>

#include "auth_claim.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/components/ffi_helpers.h"
#include "hogd/hogexec/components/ffi_types.h"
#include "types.hh"

namespace hogd {

namespace {

auto
roundtrip_auth_claim(AuthClaim const &auth_claim)
{
  auto ffi_auth_claim = FFIHelperType<generated::AuthClaim>::to_ffi(auth_claim);
  return FFIHelperType<generated::AuthClaim>::to_cpp(ffi_auth_claim);
}

} // namespace

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST(FFIHelpers, SourceAddressClaimRoundtrip)
{
  AuthClaim a(AuthClaimType::kSourceAddressAuthData);
  a.source_address_claim().allowed_keys = 1;
  a.source_address_claim().required_weight = 1;
  auto b = roundtrip_auth_claim(a);
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a, *b);

  a.source_address_claim().allowed_keys = 0x15; // bits 0, 2, 4
  a.source_address_claim().required_weight = 3;
  b = roundtrip_auth_claim(a);
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a, *b);
}

// `allowed_keys` is author-declared allow-policy that is part of the signed
// transaction body, not verified signer evidence. The syscall boundary passes
// the stored claim through verbatim -- including in-range slots that are not
// registered on the account -- so a contract reading GetAuthClaim sees exactly
// what the author committed to. Locking in this passthrough (issue #2793, WAI).
TEST(FFIHelpers, SourceAddressClaimAllowedKeysPassthroughUnchanged)
{
  constexpr uint32_t kUnregisteredSlot = 20;
  constexpr uint32_t kAllowedKeys = (1U << 0) | (1U << kUnregisteredSlot);

  AuthClaim a(AuthClaimType::kSourceAddressAuthData);
  a.source_address_claim().allowed_keys = kAllowedKeys;
  a.source_address_claim().required_weight = 1;

  auto ffi = FFIHelperType<generated::AuthClaim>::to_ffi(a);
  ASSERT_EQ(ffi.tag, generated::AuthClaim::AuthClaim_kSourceAddressAuthData);
  EXPECT_EQ(ffi.k_source_address_auth_data.source_address_claim.allowed_keys,
            kAllowedKeys);

  // Round-trips unchanged too: no slot is filtered on either conversion.
  auto b = roundtrip_auth_claim(a);
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(a, *b);
}

TEST(FFIHelpers, PubkeySignatureClaimRoundtrip)
{
  AuthClaim a(AuthClaimType::kPubkeySignature);
  a.pubkey_preimage().pubkey.scheme(CryptographicScheme::Ed25519);
  for (size_t i = 0; i < 32; i++) {
    a.pubkey_preimage().pubkey.ed25519()[i] = static_cast<uint8_t>(i + 1);
  }
  a.pubkey_preimage().nonce = 42;
  auto b = roundtrip_auth_claim(a);
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a, *b);

  a.pubkey_preimage().nonce = 0;
  b = roundtrip_auth_claim(a);
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a, *b);
}

TEST(FFIHelpers, PublicKeyRoundtrip)
{
  generated::PublicKey ffi_pk{};
  ffi_pk.ed25519.tag = generated::PublicKey::PublicKey_Ed25519;
  for (size_t i = 0; i < 32; i++) {
    ffi_pk.ed25519.ed25519[i] = static_cast<uint8_t>(i + 1);
  }
  auto cpp_pk = FFIHelperType<generated::PublicKey>::to_cpp(ffi_pk);
  ASSERT_TRUE(cpp_pk.has_value());
  ASSERT_EQ(cpp_pk->scheme(), CryptographicScheme::Ed25519);
  for (size_t i = 0; i < 32; i++) {
    ASSERT_EQ(cpp_pk->ed25519()[i], static_cast<uint8_t>(i + 1));
  }

  auto ffi_pk2 = FFIHelperType<generated::PublicKey>::to_ffi(*cpp_pk);
  ASSERT_EQ(ffi_pk2.tag(), generated::PublicKey::PublicKey_Ed25519);
  for (size_t i = 0; i < 32; i++) {
    ASSERT_EQ(ffi_pk2.ed25519.ed25519[i], static_cast<uint8_t>(i + 1));
  }
}

// The tag() accessor must read the discriminant regardless of which body
// member was last written (values may also arrive as raw bytes from guest
// memory, where no C++ union member was ever "assigned").
TEST(FFIHelpers, AuthClaimTagAccessor)
{
  AuthClaim source_claim(AuthClaimType::kSourceAddressAuthData);
  auto ffi_source = FFIHelperType<generated::AuthClaim>::to_ffi(source_claim);
  ASSERT_EQ(ffi_source.tag(),
            generated::AuthClaim::AuthClaim_kSourceAddressAuthData);

  AuthClaim pubkey_claim(AuthClaimType::kPubkeySignature);
  auto ffi_pubkey = FFIHelperType<generated::AuthClaim>::to_ffi(pubkey_claim);
  ASSERT_EQ(ffi_pubkey.tag(), generated::AuthClaim::AuthClaim_kPubkeySignature);

  generated::AuthClaim from_raw_bytes;
  std::memcpy(&from_raw_bytes, &ffi_pubkey, sizeof(from_raw_bytes));
  ASSERT_EQ(from_raw_bytes.tag(),
            generated::AuthClaim::AuthClaim_kPubkeySignature);
}

TEST(FFIHelpers, AuthClaimInvalid)
{
  generated::AuthClaim ffi_auth_claim{};
  ffi_auth_claim.k_source_address_auth_data.tag =
      static_cast<generated::AuthClaim::Tag>(-1);
  auto b = FFIHelperType<generated::AuthClaim>::to_cpp(ffi_auth_claim);
  ASSERT_FALSE(b.has_value());
}

TEST(FFIHelpers, AuthClaimInvalidTags)
{
  for (int tag_val :
       {-2, 100, 255, 1000, std::numeric_limits<int32_t>::max()}) {
    generated::AuthClaim ffi_auth_claim{};
    ffi_auth_claim.k_source_address_auth_data.tag =
        static_cast<generated::AuthClaim::Tag>(tag_val);
    auto result = FFIHelperType<generated::AuthClaim>::to_cpp(ffi_auth_claim);
    EXPECT_FALSE(result.has_value())
        << "Expected nullopt for invalid tag value " << tag_val;
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
